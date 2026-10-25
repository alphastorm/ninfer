#include "runtime/engine/checkpoint_control.h"

#include <algorithm>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using namespace ninfer::runtime;

// Instantiate only the request-settlement path of the production adapter. Other
// adapter methods remain dependent, as they do for unsupported model contracts.
struct Contract {
    struct Program {};
    struct Instance { Program* program = nullptr; };
    struct Resources {};
    struct Scheduler {};
    struct Request {
        bool finished = false;
        std::string checkpoint_session = "session";
        std::string checkpoint_tag = "unfinished-turn";
        bool is_model_finished() const { return finished; }
    };
};

template <class ModelContract>
class AdapterFixture {
    using Checkpoint = int;
    using Request = typename ModelContract::Request;
    typename ModelContract::Instance instance_;
    typename ModelContract::Resources resources_;
    typename ModelContract::Scheduler scheduler_;
    CheckpointControl checkpoints_;
    std::mutex checkpoint_provider_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> checkpoint_commands_;
    std::thread worker_;
    std::unique_lock<std::mutex>* checkpoint_execution_lock_ = nullptr;
    std::atomic<bool> admission_check_pending_{false};
    bool stopping_ = false;
    bool failed_ = false;
    bool checkpoint_cache_enabled_ = true;
    std::uint64_t next_publication_order_ = 1;
    void request_admission_check() { checkpoints_.notify_progress(); }
#include "runtime/engine/engine_checkpoint.inl"
public:
    bool settle(Request request, std::shared_ptr<PressureCheckpointHandler> handler) {
        checkpoints_.set_handler(std::move(handler));
        std::mutex execution;
        std::unique_lock lock(execution);
        checkpoint_execution_lock_ = &lock;
        return checkpoint_request_settled(request);
    }
};

void paused_request_settles_by_native_replay() {
    struct Handler final : PressureCheckpointHandler {
        unsigned calls = 0;
        void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
            ++calls;
            for (auto& victim : victims) { victim.outcome = PressureCheckpointOutcome::Pending; }
        }
    };
    auto handler = std::make_shared<Handler>();
    AdapterFixture<Contract> core;
    if (!core.settle({}, handler) || handler->calls != 0) {
        throw std::runtime_error("paused request waited for its unpublished response instead of native replay");
    }
    if (core.settle({.finished = true}, handler) || handler->calls != 1) {
        throw std::runtime_error("completed response bypassed its HTTP publication guard");
    }
}
}

int main() {
    try {
        paused_request_settles_by_native_replay();
        std::cout << "ok: paused_request_settles_by_native_replay\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
