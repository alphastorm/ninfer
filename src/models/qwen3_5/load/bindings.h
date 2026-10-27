#pragma once

#include "models/common/bindings.h"
#include "models/qwen3_5/config.h"
#include "models/common/qwen/frontend/resources.h"
#include "models/qwen3_5/weights.h"

#include <string>

namespace ninfer::models::qwen3_5::loading {

[[nodiscard]] qwen::FrontendResources bind_resources(artifact::Binder& binder,
                                                     const Config& config);

[[nodiscard]] AttentionWeights bind_attention(Bindings& bindings, const TextConfig& config,
                                              const std::string& prefix);
[[nodiscard]] DenseWeights bind_dense(Bindings& bindings, std::uint64_t hidden,
                                      std::uint64_t intermediate, const std::string& prefix,
                                      bool draft = false);
[[nodiscard]] BlockWeights bind_block(Bindings& bindings, const TextConfig& config,
                                      const std::string& prefix, MixerKind mixer);
[[nodiscard]] TextWeights bind_text(Bindings& bindings, const TextConfig& config,
                                    const LoadOptions& options);

[[nodiscard]] MtpWeights bind_mtp(Bindings& bindings, const TextConfig& config,
                                  const TextWeights& target);
[[nodiscard]] DraftWeights bind_draft(Bindings& bindings, const DraftConfig& config,
                                      const TextConfig& target, const TextWeights& weights,
                                      const std::string& component);
void bind_dflash2(Bindings& bindings, DraftWeights& weights, const DraftConfig& config,
                  const TextConfig& target);
// Native DFlash2 gate/up recipe: decode Q8 G32 to FP32, then grouped_absmax Q4 G64.
// Spans contain complete padded RowSplit rows; logical K alone selects the quantized values.
void requantize_draft_mlp_rows(std::uint64_t columns, std::uint64_t rows,
                               std::span<const std::byte> source_codes,
                               std::span<const std::byte> source_scales,
                               std::span<std::byte> target_codes,
                               std::span<std::byte> target_scales);
[[nodiscard]] ProposalWeights bind_proposal(Bindings& bindings, const artifact::Proposal& proposal,
                                            const TextConfig& target, const LoadOptions& options,
                                            std::uint32_t public_tokens);

} // namespace ninfer::models::qwen3_5::loading
