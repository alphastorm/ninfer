#pragma once

// The two process facts the runtime reads from the host, spelled once per platform.
#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <cstdio>
#else
#include <unistd.h>
#endif

namespace ninfer::core {

[[nodiscard]] inline long long process_id() noexcept {
#if defined(_WIN32)
    return static_cast<long long>(::_getpid());
#else
    return static_cast<long long>(::getpid());
#endif
}

[[nodiscard]] inline bool stderr_is_terminal() noexcept {
#if defined(_WIN32)
    return ::_isatty(::_fileno(stderr)) != 0;
#else
    return ::isatty(STDERR_FILENO) == 1;
#endif
}

} // namespace ninfer::core
