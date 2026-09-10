// The manager's graceful-stop channel on Windows: the event is created protected, a signal from
// another handle reaches the callback and is re-asserted until it takes effect, an unsignalled
// watcher is destroyed without blocking or calling back, and a squatted name is refused.
#include "serve/opaque_id.h"
#include "serve/stop_event.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

namespace {

using namespace ninfer::serve;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

std::wstring wide(const std::string& text) {
    return std::wstring(text.begin(), text.end()); // names are ASCII by construction
}

std::string fresh_name() { return new_opaque_id("Local\\NInfer-Serve-Stop-"); }

bool sid_is(PSID sid, WELL_KNOWN_SID_TYPE type) {
    return IsWellKnownSid(sid, type) != FALSE;
}

// The DACL admits exactly SYSTEM and Administrators, each with full access, and nobody else.
int check_dacl(const std::string& name) {
    HANDLE handle = OpenEventW(READ_CONTROL, FALSE, wide(name).c_str());
    if (handle == nullptr) {
        std::cerr << "could not open the stop event for READ_CONTROL (Windows error "
                  << GetLastError() << ")\n";
        return 1;
    }
    PACL dacl                       = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(handle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, &dacl, nullptr, &descriptor);
    CloseHandle(handle);
    if (status != ERROR_SUCCESS || dacl == nullptr) {
        std::cerr << "stop event has no DACL (status " << status << ")\n";
        return 1;
    }
    int failures = 0;
    failures += check(dacl->AceCount == 2, "stop event DACL does not hold exactly two ACEs");
    bool system_full = false;
    bool admins_full = false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        LPVOID ace = nullptr;
        if (!GetAce(dacl, index, &ace)) { continue; }
        const auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            failures += check(false, "stop event DACL holds a non-allow ACE");
            continue;
        }
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        PSID sid      = &allowed->SidStart;
        const bool full = (allowed->Mask & EVENT_ALL_ACCESS) == EVENT_ALL_ACCESS;
        if (sid_is(sid, WinLocalSystemSid)) {
            system_full = full;
        } else if (sid_is(sid, WinBuiltinAdministratorsSid)) {
            admins_full = full;
        } else {
            failures += check(false, "stop event DACL admits a principal other than SYSTEM/Administrators");
        }
    }
    failures += check(system_full, "SYSTEM lacks full access to the stop event");
    failures += check(admins_full, "Administrators lack full access to the stop event");
    LocalFree(descriptor);
    return failures;
}

} // namespace

int main() {
    int failures = 0;

    // A watcher that is never signalled is destroyed promptly and never calls back.
    {
        std::atomic<int> calls{0};
        const auto started = std::chrono::steady_clock::now();
        {
            StopEventWatcher watcher(fresh_name(), [&calls] {
                ++calls;
                return true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        failures += check(calls.load() == 0, "unsignalled watcher invoked its callback");
        failures += check(elapsed < std::chrono::seconds(2), "unsignalled watcher blocked its destructor");
    }

    // The manager's signal, sent through a second handle opened by name, reaches the callback,
    // which is re-asserted until it reports the stop took effect and not afterwards.
    {
        const std::string name = fresh_name();
        std::atomic<int> calls{0};
        StopEventWatcher watcher(name, [&calls] { return ++calls >= 3; });
        failures += check_dacl(name);
        HANDLE manager = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide(name).c_str());
        if (manager == nullptr) {
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                std::cerr << "SKIP: this process cannot signal a protected stop event "
                             "(not running with Administrators enabled)\n";
                return 77;
            }
            std::cerr << "could not open the stop event to signal it (Windows error "
                      << GetLastError() << ")\n";
            return 1;
        }
        failures += check(SetEvent(manager) != FALSE, "SetEvent on the manager handle failed");
        CloseHandle(manager);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (calls.load() < 3 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        failures += check(calls.load() == 3, "callback was not re-asserted until it succeeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        failures += check(calls.load() == 3, "callback was invoked again after it succeeded");
    }

    // A name that already exists is a squatter or a stale instance, never ours: refused.
    {
        const std::string name = fresh_name();
        HANDLE squatter        = CreateEventW(nullptr, TRUE, FALSE, wide(name).c_str());
        failures += check(squatter != nullptr, "could not pre-create a squatter event");
        bool refused = false;
        try {
            StopEventWatcher watcher(name, [] { return true; });
        } catch (const std::runtime_error&) { refused = true; }
        failures += check(refused, "a pre-existing stop event was adopted");
        if (squatter != nullptr) { CloseHandle(squatter); }
    }

    // Empty inputs are programming errors, not silent no-ops.
    {
        bool rejected = false;
        try {
            StopEventWatcher watcher("", [] { return true; });
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "an empty stop event name was accepted");
        rejected = false;
        try {
            StopEventWatcher watcher(fresh_name(), std::function<bool()>());
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "an empty stop callback was accepted");
    }

    if (failures == 0) { std::cout << "stop event tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
