#pragma once

#include "models/qwen4_exp/ple/input.h"
#include "core/device.h"
#include "core/tensor.h"

#include <memory>

namespace ninfer::models::qwen4_exp::ple {

struct UploadMemory {
    std::size_t ordinary_host_bytes;
    std::size_t pinned_host_bytes;
    std::size_t device_bytes;
};

// Program-owned pinned/Device E slots. One producer borrows host_slots() through its shutdown;
// it issues the tickets and cannot reuse an output until its writers and all CUDA users retire.
// This owner handles transfers and consumption, not preparation, cancellation or model state.
// Calls come from the Program thread bound to context.device(); context outlives this owner.
class UploadSlots {
public:
    UploadSlots(const DeviceContext&, std::uint32_t max_positions, std::uint32_t slots);
    ~UploadSlots();
    UploadSlots(const UploadSlots&)            = delete;
    UploadSlots& operator=(const UploadSlots&) = delete;

    [[nodiscard]] std::span<const MemorySpan> host_slots() const noexcept;
    // Requires sealed, ready input in this ticket's pinned slot. Enqueues H2D and its event.
    // Each generation uploads at most once. Input remains immutable until upload completes.
    void upload(const HostEmbedding&, CudaExecutionView transfer);
    // Caller establishes that this input is still wanted. Enqueues the current generation's
    // upload dependency, then borrows Device E; no Host wait for GPU completion occurs.
    [[nodiscard]] Tensor embedding(Ticket, CudaExecutionView consumer);
    // Exactly one ordered consumer stream; its last E reader precedes this call.
    void record_consumed(Ticket, CudaExecutionView consumer);
    [[nodiscard]] bool upload_complete(Ticket) const;
    // Retires this generation's CUDA references, or returns false while they remain. Also
    // succeeds for an input never uploaded. The caller separately drains/releases its producer.
    // Cancellation does not shorten DMA/consumer lifetime or require an artificial consumer.
    [[nodiscard]] bool try_release(Ticket);

    // Stable address for capture. Runtime use still needs embedding() and record_consumed().
    [[nodiscard]] Tensor device_slot(std::uint32_t slot, std::uint32_t tokens,
                                     std::uint32_t batch) const;
    [[nodiscard]] UploadMemory memory() const noexcept;
    [[nodiscard]] static UploadMemory memory(std::uint32_t max_positions, std::uint32_t slots);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ninfer::models::qwen4_exp::ple
