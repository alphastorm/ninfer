#include "product/logging/logging.h"
#include "product/logging/startup_log.h"
#include "serve/generation_service.h"
#include "serve/http_server.h"
#include "serve/serve_options.h"
#include "serve/stop_event.h"
#include "ninfer/build_info.h"

#include <spdlog/logger.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> g_stop_requested{false};

void handle_signal(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

} // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    const auto startup_started = Clock::now();
    const auto elapsed_ns = [](Clock::time_point begin, Clock::time_point end) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    };
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << ninfer::format_build_info("ninfer-serve") << '\n';
        return 0;
    }
    ninfer::serve::ServeOptions options;
    try {
        options = ninfer::serve::parse_serve_options(argc, argv);
    } catch (const std::invalid_argument& exception) {
        std::cerr << "ninfer-serve: " << exception.what() << '\n';
        std::cerr << ninfer::serve::serve_usage_text(argv[0]);
        return 1;
    } catch (const std::exception& exception) {
        std::cerr << "ninfer-serve: " << exception.what() << '\n';
        return 1;
    }
    if (options.help_requested) {
        std::cout << ninfer::serve::serve_usage_text(argv[0]);
        return 0;
    }

    ninfer::product::LoggingRuntime logging(
        {.logger_name  = "ninfer-serve",
         .level        = options.log_level,
         .presentation = ninfer::product::LogPresentation::Service});
    const std::shared_ptr<spdlog::logger> logger = logging.logger();
    ninfer::product::StartupLogRenderer startup_log(logging);
    ninfer::serve::OperationalLog operational_log(logger);
    bool serving = false;

    try {
        ninfer::serve::HttpServer server(options, logger);
        std::optional<ninfer::serve::GenerationService> service;
        // Install before model loading so a manager stop during startup is remembered.
        // Join the watcher before both its HTTP target and attached service are destroyed.
        std::optional<ninfer::serve::StopEventWatcher> stop_watcher;
        if (!options.stop_event.empty()) {
            stop_watcher.emplace(options.stop_event, [&server] { return server.stop(); });
        }
        if (!server.bind()) {
            operational_log.bind_failure(options.host, options.port);
            return 1;
        }

        service.emplace(options, startup_log.observer());
        startup_log.engine_ready(service->load_summary());
        operational_log.engine_capacity(*service);

        const Clock::time_point warmup_started = Clock::now();
        operational_log.warmup_started();
        try {
            service->warmup();
        } catch (const std::exception& exception) {
            const double seconds =
                std::chrono::duration<double>(Clock::now() - warmup_started).count();
            operational_log.warmup_failure(seconds, exception.what());
            return 1;
        }
        const auto warmup_finished = Clock::now();
        operational_log.warmup_complete(
            std::chrono::duration<double>(warmup_finished - warmup_started).count());
        const auto attach_started = Clock::now();
        server.attach(*service);
        const auto attach_finished = Clock::now();

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        serving = true;
        operational_log.server_ready(options.host, options.port, server.public_model_id(),
                                     !options.api_key.empty());
        startup_log.server_ready(elapsed_ns(startup_started, Clock::now()),
                                 elapsed_ns(warmup_started, warmup_finished),
                                 elapsed_ns(attach_started, attach_finished));

        std::jthread stop_monitor([&server](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (g_stop_requested.load(std::memory_order_relaxed)) {
                    server.stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        const bool ok = server.listen();
        if (!ok) {
            operational_log.listen_failure(options.host, options.port);
            return 1;
        }
        operational_log.server_stopped();
        return 0;
    } catch (const std::exception& exception) {
        operational_log.server_failure(serving, exception.what());
        return 1;
    }
}
