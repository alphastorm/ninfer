#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ninfer::runtime {

struct ContinuationCheckpointReadRequest {
    std::uint64_t file_offset = 0;
    std::span<std::byte> destination;
};

class ContinuationCheckpointReadCompletion {
public:
    virtual ~ContinuationCheckpointReadCompletion() = default;
    virtual void wait()                             = 0;
};

class ContinuationCheckpointReadQueue {
public:
    virtual ~ContinuationCheckpointReadQueue() = default;

    [[nodiscard]] virtual std::string_view backend_name() const noexcept       = 0;
    [[nodiscard]] virtual bool available() const noexcept                      = 0;
    [[nodiscard]] virtual std::string_view unavailable_reason() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const ContinuationCheckpointReadRequest> requests) = 0;
};

class ContinuationCheckpointWriter {
public:
    virtual ~ContinuationCheckpointWriter()                   = default;
    virtual bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                            std::span<const std::byte> bytes) = 0;
};

class ContinuationCheckpointReader {
public:
    virtual ~ContinuationCheckpointReader() = default;
    [[nodiscard]] virtual std::optional<std::uint64_t> file_size(std::string_view path) const = 0;
    virtual bool read_file(std::string_view path, std::uint64_t offset,
                           std::span<std::byte> destination) const                            = 0;
};

struct ContinuationCheckpointStats {
    std::uint32_t frontier_tokens = 0;
    std::uint32_t restored_tokens = 0;
    std::uint64_t payload_bytes   = 0;

    [[nodiscard]] friend constexpr bool operator==(ContinuationCheckpointStats,
                                                   ContinuationCheckpointStats) noexcept = default;
};

} // namespace ninfer::runtime
