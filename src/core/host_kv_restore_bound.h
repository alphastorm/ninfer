#pragma once

// Restore materialises a continuation's KV into the host pool, so the pool - not the device KV
// arena - bounds the largest session a configuration can admit back. The bound lives here, free of
// CUDA headers, so the export gate, the startup summary, and a host-only test all share one
// definition: a pool smaller than a session's KV turns a reported save into a session that answers
// previous_response_not_found after a restart (alphastorm/omp-ninfer#42).

#include <cstddef>
#include <cstdint>

namespace ninfer {

[[nodiscard]] constexpr std::uint64_t
host_kv_bytes_for_kv_pages(std::uint32_t pages, std::size_t text_page_stride,
                           std::size_t backend_page_stride) noexcept {
    return static_cast<std::uint64_t>(pages) *
           (static_cast<std::uint64_t>(text_page_stride) +
            static_cast<std::uint64_t>(backend_page_stride));
}

[[nodiscard]] constexpr std::uint32_t
host_kv_restorable_tokens(std::size_t capacity_bytes, std::size_t text_page_stride,
                          std::size_t backend_page_stride, std::uint32_t page_size,
                          std::uint32_t kv_capacity) noexcept {
    const std::uint64_t per_page = static_cast<std::uint64_t>(text_page_stride) +
                                   static_cast<std::uint64_t>(backend_page_stride);
    if (per_page == 0 || page_size == 0) { return 0U; }
    const std::uint64_t pages  = static_cast<std::uint64_t>(capacity_bytes) / per_page;
    const std::uint64_t tokens = pages * static_cast<std::uint64_t>(page_size);
    return static_cast<std::uint32_t>(tokens < kv_capacity ? tokens : kv_capacity);
}

} // namespace ninfer
