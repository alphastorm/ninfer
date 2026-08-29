#include "serve/automatic_checkpoint_queue.h"

#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

int main() {
    std::promise<void> checkpoint_started;
    std::promise<void> release_checkpoint;
    std::shared_future<void> release_future = release_checkpoint.get_future().share();
    std::mutex saved_mutex;
    std::vector<std::string> saved;
    ninfer::serve::AutomaticCheckpointQueue automatic_checkpoints(
        [&](std::string_view digest) {
            if (digest == "session-a") { checkpoint_started.set_value(); }
            release_future.wait();
            std::lock_guard lock(saved_mutex);
            saved.emplace_back(digest);
        },
        2);

    const auto enqueue_started = std::chrono::steady_clock::now();
    automatic_checkpoints.enqueue("session-a");
    const auto enqueue_elapsed = std::chrono::steady_clock::now() - enqueue_started;
    if (checkpoint_started.get_future().wait_for(std::chrono::seconds(1)) !=
            std::future_status::ready ||
        enqueue_elapsed >= std::chrono::milliseconds(250)) {
        std::cerr << "automatic checkpoint save blocked request completion\n";
        return 1;
    }

    automatic_checkpoints.enqueue("session-a");
    automatic_checkpoints.enqueue("session-b");
    release_checkpoint.set_value();
    automatic_checkpoints.drain();
    if (saved != std::vector<std::string>{"session-a", "session-b"}) {
        std::cerr << "automatic checkpoint queue did not coalesce and drain sessions\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
