#include "serve/opaque_id.h"
#include "serve/stop_event.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
std::wstring wide(const std::string& text) { return {text.begin(), text.end()}; }
std::string fresh_name() { return new_opaque_id("Local\\NInfer-Serve-Stop-"); }
int check_dacl(const std::string& name) {
    HANDLE handle = OpenEventW(READ_CONTROL, FALSE, wide(name).c_str());
    if (!handle) { return check(false, "cannot inspect stop event DACL"); }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(handle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, &dacl, nullptr, &descriptor);
    CloseHandle(handle);
    if (status != ERROR_SUCCESS || !dacl) {
        if (descriptor) { LocalFree(descriptor); }
        return check(false, "stop event has no inspectable DACL");
    }
    int failures = check(dacl->AceCount == 2, "DACL must contain exactly two ACEs");
    bool system_full = false, admins_full = false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        LPVOID ace = nullptr;
        if (!GetAce(dacl, index, &ace)) { ++failures; continue; }
        const auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ++failures; continue; }
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        PSID sid = &allowed->SidStart;
        const bool full = (allowed->Mask & EVENT_ALL_ACCESS) == EVENT_ALL_ACCESS;
        if (IsWellKnownSid(sid, WinLocalSystemSid)) { system_full = full; }
        else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) { admins_full = full; }
        else { ++failures; }
    }
    failures += check(system_full && admins_full, "DACL must admit only SYSTEM/Administrators");
    LocalFree(descriptor);
    return failures;
}
} // namespace

int main() {
    int failures = 0;
    {
        std::atomic<int> calls{0};
        const auto started = std::chrono::steady_clock::now();
        {
            StopEventWatcher watcher(fresh_name(), [&] { ++calls; return true; });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        failures += check(calls == 0, "unsignalled watcher called back");
        failures += check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                          "unsignalled watcher blocked destruction");
    }
    {
        const auto name = fresh_name();
        std::atomic<int> calls{0};
        StopEventWatcher watcher(name, [&] { return ++calls >= 3; });
        failures += check_dacl(name);
        HANDLE manager = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide(name).c_str());
        if (!manager && GetLastError() == ERROR_ACCESS_DENIED) {
            std::cerr << "SKIP: protected event requires Administrators enabled\n";
            return 77;
        }
        if (!manager) { return 1; }
        failures += check(SetEvent(manager) != FALSE, "manager could not signal stop");
        CloseHandle(manager);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (calls < 3 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        failures += check(calls == 3, "stop was not retried until acknowledged");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        failures += check(calls == 3, "acknowledged stop was retried");
    }
    {
        const auto name = fresh_name();
        HANDLE squatter = CreateEventW(nullptr, TRUE, FALSE, wide(name).c_str());
        failures += check(squatter != nullptr, "cannot create squatter fixture");
        bool refused = false;
        try { StopEventWatcher watcher(name, [] { return true; }); }
        catch (const std::runtime_error&) { refused = true; }
        failures += check(refused, "squatted stop event was adopted");
        if (squatter) { CloseHandle(squatter); }
    }
    bool rejected = false;
    try { StopEventWatcher watcher("", [] { return true; }); }
    catch (const std::invalid_argument&) { rejected = true; }
    failures += check(rejected, "empty stop name accepted");
    rejected = false;
    try { StopEventWatcher watcher(fresh_name(), {}); }
    catch (const std::invalid_argument&) { rejected = true; }
    failures += check(rejected, "empty stop callback accepted");
    return failures == 0 ? 0 : 1;
}
#else
int main() {
    try {
        ninfer::serve::StopEventWatcher watcher("unsupported", [] { return true; });
    } catch (const std::logic_error&) { return 0; }
    std::cerr << "non-Windows stop event was not refused\n";
    return 1;
}
#endif
