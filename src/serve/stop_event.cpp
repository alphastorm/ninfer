#include "serve/stop_event.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#endif

namespace ninfer::serve {
#if defined(_WIN32)

struct StopEventWatcher::State {
    HANDLE stop_event = nullptr;
    HANDLE exit_event = nullptr;
    std::function<bool()> on_stop;
    std::thread thread;
    ~State() {
        if (stop_event) { CloseHandle(stop_event); }
        if (exit_event) { CloseHandle(exit_event); }
    }
    void run() noexcept {
        const HANDLE handles[] = {stop_event, exit_event};
        const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (woke == WAIT_OBJECT_0 + 1) { return; }
        try {
            if (woke != WAIT_OBJECT_0) {
                spdlog::error("stop event wait failed (Windows error {}); stopping", GetLastError());
            } else {
                spdlog::info("stop requested by the manager; finishing in-flight requests");
            }
            while (!on_stop()) {
                if (WaitForSingleObject(exit_event, 50) == WAIT_OBJECT_0) { return; }
            }
        } catch (const std::exception& error) {
            try { spdlog::error("stop request failed: {}", error.what()); } catch (...) {}
        } catch (...) {
            try { spdlog::error("stop request failed"); } catch (...) {}
        }
    }
};

namespace {
std::string last_error(const char* operation) {
    return std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ')';
}
std::wstring to_wide(const std::string& text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("stop event name is too long");
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) { throw std::invalid_argument("stop event name is not valid UTF-8"); }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            wide.data(), length) != length) {
        throw std::runtime_error(last_error("stop event UTF-8 conversion"));
    }
    return wide;
}
HANDLE protected_event(const std::wstring& name) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",
                                                              SDDL_REVISION_1, &descriptor, nullptr)) {
        throw std::runtime_error(last_error("stop event security descriptor"));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    HANDLE event = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!event) {
        SetLastError(error);
        throw std::runtime_error(last_error("stop event creation"));
    }
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(event);
        throw std::runtime_error("stop event already exists; mint a fresh name for every launch");
    }
    return event;
}
} // namespace

StopEventWatcher::StopEventWatcher(const std::string& name, std::function<bool()> on_stop)
    : state_(std::make_unique<State>()) {
    if (name.empty()) { throw std::invalid_argument("stop event name must not be empty"); }
    if (!on_stop) { throw std::invalid_argument("stop event callback must not be empty"); }
    state_->on_stop = std::move(on_stop);
    state_->exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state_->exit_event) { throw std::runtime_error(last_error("stop watcher exit event")); }
    state_->stop_event = protected_event(to_wide(name));
    state_->thread = std::thread([state = state_.get()] { state->run(); });
}
StopEventWatcher::~StopEventWatcher() {
    if (state_->thread.joinable()) {
        SetEvent(state_->exit_event);
        state_->thread.join();
    }
}

#else
struct StopEventWatcher::State {};
StopEventWatcher::StopEventWatcher(const std::string&, std::function<bool()>) {
    throw std::logic_error("stop events require Windows");
}
StopEventWatcher::~StopEventWatcher() = default;
#endif
} // namespace ninfer::serve
