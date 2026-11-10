#pragma once

#include "core/paged_kv_cache.h"
#include "ninfer/types.h"

#include <cstdint>
#include <stdexcept>

namespace ninfer::test::qwen3_5 {

// The fork has separate KV-page and State quotas, not upstream's unified Context arena.
// Both initial working sets must fit, and either complete request must fit on its own.
[[nodiscard]] inline std::uint32_t
configure_two_request_pressure(EngineOptions& options, std::uint32_t output_tokens,
                               bool single_request_growth = false) {
    if (options.max_concurrency != 2 || options.context_cache.enabled)
        throw std::invalid_argument("pressure fixture requires two uncached requests");
    constexpr auto page_tokens = static_cast<std::uint32_t>(kPagedKVPageSize);
    const auto pages = [](std::uint32_t tokens) {
        return (tokens + page_tokens - 1U) / page_tokens;
    };
    const auto draft = options.speculative.backend == SpeculativeBackend::None
                           ? 0U : options.speculative.draft_tokens;
    if (output_tokens <= draft + 1U)
        throw std::invalid_argument("pressure fixture requires a growing output budget");
    for (std::uint32_t capacity = 512; capacity <= 65536; capacity *= 2) {
        const auto available = pages(capacity);
        for (std::uint32_t prompt = 128;
             prompt + output_tokens + draft + page_tokens + 1U <= capacity;
             prompt += 128) {
            // Include a speculative unit and one private-tail COW page for both admissions.
            if (2U * pages(prompt + draft + page_tokens + 1U) > available) break;
            const auto grown = pages(prompt + output_tokens - draft - 1U);
            const auto peer = single_request_growth ? pages(prompt) : grown;
            if (grown + peer <= available) continue;
            options.max_context = capacity;
            options.kv_capacity = KvCapacityPolicy::explicit_capacity(capacity);
            return prompt;
        }
    }
    throw std::runtime_error("could not form a two-request KV pressure workload");
}

} // namespace ninfer::test::qwen3_5
