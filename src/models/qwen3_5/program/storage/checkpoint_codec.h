#pragma once

#include "runtime/contract/durable_engine_api.h"

#include <cstddef>
#include <span>
#include <vector>

namespace ninfer::models::qwen3_5::detail::checkpoint_codec {

// Canonical field-wise K1 CBOR. Decoding rejects noncanonical, duplicate and unknown fields by
// round-tripping the bounded typed document; native geometry is checked by Program before reserve.
[[nodiscard]] std::vector<std::uint8_t> encode(const runtime::DurableCheckpointSet&);
[[nodiscard]] runtime::DurableCheckpointSet decode(std::span<const std::byte>,
                                                  const runtime::DurableLimits&);
void validate(const runtime::DurableCheckpointSet&, const runtime::DurableLimits&);

} // namespace ninfer::models::qwen3_5::detail::checkpoint_codec
