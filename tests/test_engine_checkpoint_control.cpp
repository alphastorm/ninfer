#include "runtime/engine/checkpoint_control.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace ninfer::runtime;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

void save_coordination_does_not_block_pressure() {
    CheckpointControl control;
    std::mutex coordination;
    struct Handler final : PressureCheckpointHandler {
        std::mutex& coordination;
        explicit Handler(std::mutex& mutex) : coordination(mutex) {}
        void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
            std::unique_lock lock(coordination, std::try_to_lock);
            for (auto& victim : victims) {
                victim.outcome = lock.owns_lock() ? PressureCheckpointOutcome::Settled
                                                 : PressureCheckpointOutcome::Pending;
            }
        }
    };
    control.set_handler(std::make_shared<Handler>(coordination));
    std::unique_lock saving(coordination);
    auto pressure = std::async(std::launch::async, [&] {
        PressureCheckpointVictim victim{"session", "turn"};
        return control.settle({&victim, 1});
    });
    const bool ready = pressure.wait_for(2s) == std::future_status::ready;
    saving.unlock();
    require(ready && !pressure.get(), "save coordination blocked worker pressure instead of Pending");
    PressureCheckpointVictim victim{"session", "turn"};
    require(!control.settle({&victim, 1}), "Pending pressure retried without checkpoint progress");
    control.notify_progress();
    require(control.settle({&victim, 1}), "checkpoint progress did not settle the next pressure plan");
}

void handler_removal_keeps_callback_alive() {
    CheckpointControl control;
    bool destroyed = false;
    bool completed = false;
    struct Handler final : PressureCheckpointHandler {
        CheckpointControl& control;
        bool& destroyed;
        bool& completed;
        Handler(CheckpointControl& c, bool& d, bool& done) : control(c), destroyed(d), completed(done) {}
        ~Handler() override { destroyed = true; }
        void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
            control.set_handler({});
            completed = !destroyed;
            for (auto& victim : victims) { victim.outcome = PressureCheckpointOutcome::Pending; }
        }
    };
    control.set_handler(std::make_shared<Handler>(control, destroyed, completed));
    PressureCheckpointVictim victim{"session", "turn"};
    require(!control.settle({&victim, 1}) && completed && destroyed && control.pending(),
            "removing the installed handler destroyed its in-flight callback");
    control.begin_turn();
    require(!control.pending() && control.settle({&victim, 1}),
            "callback-time provider progress was lost or erased the current Pending result");
}

void publication_and_restore_share_one_gate() {
    CheckpointControl control;
    const auto state = control.session("session");
    state->epoch = {7};
    state->tag = "turn";
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> disk_published = false;
    auto publication = std::async(std::launch::async, [&] {
        return control.publish("session", "turn", {7}, [&] {
            entered.set_value();
            released.wait();
            disk_published = true;
            return true;
        });
    });
    entered.get_future().get();
    std::promise<void> restoring;
    auto restore = std::async(std::launch::async, [&] {
        restoring.set_value();
        std::string tag = "restored";
        return control.commit_import(state, {7}, tag, 8, [&] {
            require(disk_published, "native restore published while disk current was unfinished");
        });
    });
    restoring.get_future().get();
    release.set_value();
    require(publication.get().state == DurableExportState::Exported && restore.get(),
            "publication finishing during restore did not serialize");
    require(control.epoch("session").value == 8, "restore did not install a fresh runtime order");
    bool called = false;
    const auto stale = control.publish("session", "turn", {7}, [&] { called = true; return true; });
    require(stale.state == DurableExportState::Stale && !called,
            "stale captured export epoch invoked current replacement");
    std::string old_tag = "old-restore";
    require(!control.commit_import(state, {7}, old_tag, 9, [&] { called = true; }) && !called,
            "stale prepared import mutated native ownership");
}

void publication_failure_is_not_acknowledged() {
    CheckpointControl control;
    const auto state = control.session("session");
    state->epoch = {1};
    state->tag = "turn";
    const auto failed = control.publish("session", "turn", {1}, []() -> bool {
        throw std::runtime_error("injected current sync failure");
    });
    require(failed.state == DurableExportState::Refused && failed.reason == DurableReason::IoUnavailable,
            "failed final publication was acknowledged");
}
} // namespace

int main() {
    try {
        save_coordination_does_not_block_pressure();
        handler_removal_keeps_callback_alive();
        publication_and_restore_share_one_gate();
        publication_failure_is_not_acknowledged();
        std::cout << "ok: checkpoint coordination, handler lifetime, restore publication, stale epoch\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
