#pragma once

#if defined(_MSC_VER)
#define NINFER_UNREACHABLE() __assume(0)
#elif defined(__GNUC__) || defined(__clang__)
#define NINFER_UNREACHABLE() __builtin_unreachable()
#else
#include <cstdlib>
#define NINFER_UNREACHABLE() std::abort()
#endif
