#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ninfer::serve {

// Named manual-reset event; SYSTEM/Administrators only. A launch must mint a fresh name. A
// signalled request is retried every 50 ms until the accept loop observes it or destruction wakes
// the watcher. Destroy the watcher before destroying its callback target.
class StopEventWatcher {
public:
    StopEventWatcher(const std::string& name, std::function<bool()> on_stop);
    ~StopEventWatcher();
    StopEventWatcher(const StopEventWatcher&) = delete;
    StopEventWatcher& operator=(const StopEventWatcher&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ninfer::serve
