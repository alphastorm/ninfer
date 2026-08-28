#pragma once

#include "runtime/contract/checkpoint_io.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ninfer::runtime {

enum class LinuxCheckpointEnvironment : std::uint8_t {
    NativeLinux,
    Wsl,
};

struct IoUringCheckpointCapability {
    bool available                         = false;
    LinuxCheckpointEnvironment environment = LinuxCheckpointEnvironment::NativeLinux;
    std::size_t memory_alignment           = 0;
    std::size_t offset_alignment           = 0;
    std::string reason;
};

// Probes the exact root filesystem, native io_uring operations, O_DIRECT alignment, fsync, and
// rename-exchange semantics used by IoUringCheckpointBackend. The root must already exist.
[[nodiscard]] IoUringCheckpointCapability
probe_io_uring_checkpoint_capability(const std::filesystem::path& root) noexcept;

struct IoUringCheckpointLimits {
    std::uint64_t max_payload_bytes       = 1ULL << 40U;
    std::uint64_t max_total_payload_bytes = 4ULL << 40U;
    std::uint32_t lock_timeout_ms         = 30'000;
};

class IoUringCheckpointBackend final : public CheckpointBackend {
public:
    // Construction fails unless root is an existing directory on supported local storage and every
    // required native io_uring/O_DIRECT durability capability succeeds. There is no buffered or
    // synchronous payload-I/O fallback.
    explicit IoUringCheckpointBackend(std::filesystem::path root,
                                      IoUringCheckpointLimits limits = IoUringCheckpointLimits{});
    ~IoUringCheckpointBackend() override;

    IoUringCheckpointBackend(const IoUringCheckpointBackend&)            = delete;
    IoUringCheckpointBackend& operator=(const IoUringCheckpointBackend&) = delete;
    IoUringCheckpointBackend(IoUringCheckpointBackend&&)                 = delete;
    IoUringCheckpointBackend& operator=(IoUringCheckpointBackend&&)      = delete;

    [[nodiscard]] CheckpointBackendCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::uint64_t committed_generation() const noexcept override;
    void stage(const CheckpointManifestV1& manifest, std::span<const CheckpointPayload> payloads,
               const CheckpointStageKey& key) override;
    void commit(const CheckpointStageKey& key) override;
    void abort(const CheckpointStageKey& key) noexcept override;
    CheckpointImage load(const CheckpointExpectation& expected) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#if defined(NINFER_IO_URING_TESTING)
void io_uring_checkpoint_test_fail_next_submitted_batch() noexcept;
void io_uring_checkpoint_test_fail_publication_and_rollback_fsync() noexcept;
#endif

} // namespace ninfer::runtime
