#pragma once

// SHA-256 compression on the x86 SHA extensions (SHA-NI). Restore verification hashes every
// checkpoint payload byte, so the scalar schedule bounded a 4.5 GB restore at roughly 10 s per
// pass; the instruction path runs the same compression several times faster on one core.
// Runtime-dispatched from Sha256: available() is a cpuid probe, process_blocks() requires it.

#include <array>
#include <cstddef>
#include <cstdint>

namespace ninfer::crypto::x86 {

[[nodiscard]] bool sha256_available() noexcept;

// Compresses `blocks` consecutive 64-byte blocks into `state`. Callers must check availability.
void sha256_process_blocks(std::array<std::uint32_t, 8>& state, const std::byte* data,
                           std::size_t blocks) noexcept;

} // namespace ninfer::crypto::x86
