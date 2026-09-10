#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ninfer::serve {

// A manager's graceful-stop request on Windows, where a service-like process receives no SIGTERM
// and, run by Task Scheduler, has no console for Ctrl events. The manager mints one name per
// launch (parse_serve_options fixes its shape), the server creates the event here, and the
// manager signals it. Creation refuses a name that already exists: the manager never reuses one,
// so an existing object was put there by someone else. The event's DACL admits only SYSTEM and
// Administrators, the principals that could already terminate the process.
//
// One thread waits on the event. When it is signalled the thread invokes `on_stop` and, while
// `on_stop` reports that the request has not taken effect, again every 50 ms until it does or
// the watcher is destroyed: an accept loop cannot be stopped before it starts (HttpServer::stop),
// so a request that lands during model load is re-asserted until the server is listening.
class StopEventWatcher {
public:
    StopEventWatcher(const std::string& name, std::function<bool()> on_stop);
    // Wakes and joins the waiter without invoking `on_stop`.
    ~StopEventWatcher();
    StopEventWatcher(const StopEventWatcher&)            = delete;
    StopEventWatcher& operator=(const StopEventWatcher&) = delete;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ninfer::serve
