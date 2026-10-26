#pragma once

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <exception>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace checkpoint_write_fault {
inline std::atomic<int> generic_error{0};
inline std::atomic<unsigned long> windows_error{0};
inline std::atomic<unsigned> fired{0};

struct Scope {
    Scope(int generic, unsigned long windows = 0) {
        fired = 0;
        generic_error = generic;
        windows_error = windows;
    }
    ~Scope() { generic_error = 0; windows_error = 0; }
    Scope(const Scope&) = delete;
};

inline bool refuse_stdio_flush(std::FILE* file) {
    const int error = generic_error.load();
    if (!error || file == stdin || file == stdout || file == stderr) { return false; }
    ++fired;
    errno = error;
    return true;
}

#ifdef _WIN32
inline int __cdecl flush(std::FILE* file) {
    if (refuse_stdio_flush(file)) { return EOF; }
#ifdef _DEBUG
    constexpr auto library = L"ucrtbased.dll";
#else
    constexpr auto library = L"ucrtbase.dll";
#endif
    static const auto real_flush = reinterpret_cast<int (__cdecl*)(std::FILE*)>(
        GetProcAddress(GetModuleHandleW(library), "fflush"));
    if (!real_flush) { std::terminate(); }
    return real_flush(file);
}
#endif
} // namespace checkpoint_write_fault

#ifdef _WIN32
extern "C" int (__cdecl* __imp_fflush)(std::FILE*) = &checkpoint_write_fault::flush;
#elif defined(__linux__)
extern "C" int __real_fflush(std::FILE*);
extern "C" int __wrap_fflush(std::FILE* file) {
    if (checkpoint_write_fault::refuse_stdio_flush(file)) { return EOF; }
    return __real_fflush(file);
}
#endif
