#include "product/logging/logging.h"
#include "core/restore_trace.h"
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
#include <utility>
#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace {

std::atomic<ninfer::serve::HttpServer*> g_server{nullptr};

void handle_signal(int) {
    ninfer::serve::HttpServer* server = g_server.load();
    if (server != nullptr) { server->stop(); }
}

} // namespace

int main(int argc, char** argv) {
    ninfer::restore_trace::event("process.start");
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
        // Install before model loading so a manager stop during startup is remembered.
        // This watcher is destroyed (and joined) before its HTTP callback target.
        std::optional<ninfer::serve::StopEventWatcher> stop_watcher;
        if (!options.stop_event.empty()) {
            stop_watcher.emplace(options.stop_event, [&server] { return server.stop(); });
        }
        ninfer::restore_trace::event("bind.begin");
        if (!server.bind()) {
            operational_log.bind_failure(options.host, options.port);
            return 1;
        }
        ninfer::restore_trace::event("bind.end");

        ninfer::serve::GenerationService service(options, startup_log.observer());
#if defined(__linux__)
        if (auto query = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "restore_cuda_loading_mode")))
            query();
#endif
        startup_log.engine_ready(service.load_summary());
        ninfer::restore_trace::event("engine.ready");
        operational_log.engine_capacity(service);

        using Clock                            = std::chrono::steady_clock;
        const Clock::time_point warmup_started = Clock::now();
        operational_log.warmup_started();
        try {
            ninfer::restore_trace::event("warmup.begin");
            service.warmup();
            ninfer::restore_trace::event("warmup.end");
        } catch (const std::exception& exception) {
            const double seconds =
                std::chrono::duration<double>(Clock::now() - warmup_started).count();
            operational_log.warmup_failure(seconds, exception.what());
            return 1;
        }
        operational_log.warmup_complete(
            std::chrono::duration<double>(Clock::now() - warmup_started).count());
        ninfer::restore_trace::event("checkpoint.attach.begin");
        server.attach(service);
        ninfer::restore_trace::event("checkpoint.attach.end");

        g_server.store(&server);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        serving = true;
        operational_log.server_ready(options.host, options.port, server.public_model_id(),
                                     !options.api_key.empty());

        const bool ok = server.listen();
        g_server.store(nullptr);
        if (!ok) {
            operational_log.listen_failure(options.host, options.port);
            return 1;
        }
        operational_log.server_stopped();
        return 0;
    } catch (const std::exception& exception) {
        g_server.store(nullptr);
        operational_log.server_failure(serving, exception.what());
        return 1;
    }
}
