#pragma once

#include "runtime/contract/checkpoint_io.h"
#include "runtime/contract/continuation_checkpoint.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ninfer::runtime::windows {

struct DirectStorageCheckpointConfig {
    std::filesystem::path directory;
    std::uint64_t max_checkpoint_bytes = 64ULL << 30;
    std::uint32_t lock_timeout_ms      = 30'000;
    std::uint32_t io_timeout_ms        = 30'000;
    std::size_t max_cleanup_entries    = 4'096;
};

class CheckpointDirectoryLock {
public:
    virtual ~CheckpointDirectoryLock() = default;
};

class CheckpointFileSystem {
public:
    using PathPredicate = bool (*)(const std::filesystem::path&);

    virtual ~CheckpointFileSystem() = default;

    virtual void ensure_directory(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual std::unique_ptr<CheckpointDirectoryLock>
    lock_directory(const std::filesystem::path& path, std::uint32_t timeout_ms) = 0;
    [[nodiscard]] virtual std::vector<std::filesystem::path>
    list_regular_files(const std::filesystem::path& path, std::size_t max_matches,
                       PathPredicate predicate)                                            = 0;
    [[nodiscard]] virtual bool file_exists(const std::filesystem::path& path)              = 0;
    [[nodiscard]] virtual std::uint64_t file_size(const std::filesystem::path& path)       = 0;
    virtual void read_exact(const std::filesystem::path& path, std::span<std::byte> bytes) = 0;
    virtual void write_bytes_durable(const std::filesystem::path& path,
                                     std::span<const std::byte> bytes)                     = 0;
    virtual void write_payloads_durable(const std::filesystem::path& path,
                                        std::span<const CheckpointPayload> payloads,
                                        std::span<const std::uint64_t> payload_offsets,
                                        std::uint64_t total_bytes)                         = 0;
    // The replacement is the commit point: success is durable, while a throw leaves the
    // destination authoritative and unchanged.
    virtual void atomic_replace_durable(const std::filesystem::path& source,
                                        const std::filesystem::path& destination)      = 0;
    [[nodiscard]] virtual bool remove_file(const std::filesystem::path& path) noexcept = 0;
};

class DirectStorageCheckpointBackend final : public CheckpointBackend {
public:
    DirectStorageCheckpointBackend(DirectStorageCheckpointConfig config,
                                   std::shared_ptr<CheckpointFileSystem> file_system,
                                   std::shared_ptr<ContinuationCheckpointReadQueue> read_queue);
    ~DirectStorageCheckpointBackend() override;

    DirectStorageCheckpointBackend(const DirectStorageCheckpointBackend&)            = delete;
    DirectStorageCheckpointBackend& operator=(const DirectStorageCheckpointBackend&) = delete;
    DirectStorageCheckpointBackend(DirectStorageCheckpointBackend&&)                 = delete;
    DirectStorageCheckpointBackend& operator=(DirectStorageCheckpointBackend&&)      = delete;

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

#if defined(_WIN32)
[[nodiscard]] std::shared_ptr<ContinuationCheckpointReadQueue>
make_direct_storage_checkpoint_read_queue(const std::filesystem::path& root,
                                          std::uint32_t timeout_ms);
[[nodiscard]] std::unique_ptr<DirectStorageCheckpointBackend>
make_direct_storage_checkpoint_backend(DirectStorageCheckpointConfig config);
#endif

} // namespace ninfer::runtime::windows
