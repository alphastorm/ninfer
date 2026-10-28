#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace checkpoint_write_fault {
inline std::atomic<int> generic_error{0};
inline std::atomic<unsigned long> windows_error{0};
inline std::atomic<unsigned> fired{0};
// A refused close still releases the stream, then reports close_error. Only streams whose
// final path component starts with close_name are refused.
inline std::atomic<int> close_error{0};
inline std::atomic<const char*> close_name{nullptr};

struct Scope {
    Scope(int generic, unsigned long windows = 0) {
        fired = 0;
        generic_error = generic;
        windows_error = windows;
    }
    ~Scope() { generic_error = 0; windows_error = 0; }
    Scope(const Scope&) = delete;
};

struct CloseScope {
    CloseScope(int error, const char* name) {
        fired = 0;
        close_name = name;
        close_error = error;
    }
    ~CloseScope() { close_error = 0; close_name = nullptr; }
    CloseScope(const CloseScope&) = delete;
};

inline bool refuse_stdio_flush(std::FILE* file) {
    const int error = generic_error.load();
    if (!error || file == stdin || file == stdout || file == stderr) { return false; }
    ++fired;
    errno = error;
    return true;
}

inline bool stream_named(std::FILE* file, std::string_view prefix) {
#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    if (handle == INVALID_HANDLE_VALUE) { return false; }
    std::wstring path(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, path.data(),
                                                   static_cast<DWORD>(path.size()),
                                                   FILE_NAME_NORMALIZED);
    if (length == 0 || length >= path.size()) { return false; }
    path.resize(length);
    const std::wstring_view leaf = std::wstring_view(path).substr(path.find_last_of(L"\\/") + 1);
    return leaf.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), leaf.begin(), [](char expected, wchar_t actual) {
               return static_cast<wchar_t>(static_cast<unsigned char>(expected)) == actual;
           });
#elif defined(__linux__)
    const std::string link = "/proc/self/fd/" + std::to_string(::fileno(file));
    std::string path(4096, '\0');
    const auto length = ::readlink(link.c_str(), path.data(), path.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) { return false; }
    path.resize(static_cast<std::size_t>(length));
    return std::string_view(path).substr(path.rfind('/') + 1).starts_with(prefix);
#else
    (void)file;
    (void)prefix;
    return false;
#endif
}

inline bool refuse_stdio_close(std::FILE* file) {
    const char* const name = close_name.load();
    if (!close_error.load() || name == nullptr || file == stdin || file == stdout ||
        file == stderr || !stream_named(file, name)) {
        return false;
    }
    ++fired;
    return true;
}

#ifdef _WIN32
#ifdef _DEBUG
inline constexpr const wchar_t* crt_library = L"ucrtbased.dll";
#else
inline constexpr const wchar_t* crt_library = L"ucrtbase.dll";
#endif

inline int __cdecl flush(std::FILE* file) {
    if (refuse_stdio_flush(file)) { return EOF; }
    static const auto real_flush = reinterpret_cast<int (__cdecl*)(std::FILE*)>(
        GetProcAddress(GetModuleHandleW(crt_library), "fflush"));
    if (!real_flush) { std::terminate(); }
    return real_flush(file);
}

inline int __cdecl close_stream(std::FILE* file) {
    const bool refuse = refuse_stdio_close(file);
    static const auto real_close = reinterpret_cast<int (__cdecl*)(std::FILE*)>(
        GetProcAddress(GetModuleHandleW(crt_library), "fclose"));
    if (!real_close) { std::terminate(); }
    const int result = real_close(file);
    if (!refuse) { return result; }
    errno = close_error.load();
    return EOF;
}
#endif
} // namespace checkpoint_write_fault

#ifdef _WIN32
extern "C" int (__cdecl* __imp_fflush)(std::FILE*) = &checkpoint_write_fault::flush;
extern "C" int (__cdecl* __imp_fclose)(std::FILE*) = &checkpoint_write_fault::close_stream;
#elif defined(__linux__)
extern "C" int __real_fflush(std::FILE*);
extern "C" int __wrap_fflush(std::FILE* file) {
    if (checkpoint_write_fault::refuse_stdio_flush(file)) { return EOF; }
    return __real_fflush(file);
}
extern "C" int __real_fclose(std::FILE*);
extern "C" int __wrap_fclose(std::FILE* file) {
    const bool refuse = checkpoint_write_fault::refuse_stdio_close(file);
    const int result = __real_fclose(file);
    if (!refuse) { return result; }
    errno = checkpoint_write_fault::close_error.load();
    return EOF;
}
#endif
