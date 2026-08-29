#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include "runtime/platform/linux/io_uring_checkpoint_backend.h"

#include <linux/io_uring.h>
#include <linux/stat.h>

#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef STATX_DIOALIGN
#    define STATX_DIOALIGN 0x00002000U
#endif

#ifndef RENAME_NOREPLACE
#    define RENAME_NOREPLACE (1U << 0U)
#endif

#ifndef RENAME_EXCHANGE
#    define RENAME_EXCHANGE (1U << 1U)
#endif

namespace ninfer::runtime {
namespace {

using crypto::sha256;
using crypto::sha256_hex;

constexpr std::size_t kPayloadKindCount  = 4;
constexpr std::size_t kManifestBaseBytes = 164;
constexpr std::size_t kDescriptorBytes   = 41;
constexpr std::size_t kMaximumManifestBytes =
    kManifestBaseBytes + kPayloadKindCount * kDescriptorBytes;
constexpr unsigned kRequestedRingEntries         = 16;
constexpr std::size_t kIoQueueDepth              = 8;
constexpr std::size_t kDirectChunkBytes          = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumDioAlignment       = 1U * 1024U * 1024U;
constexpr std::string_view kPublicationUncertain = ".publication-uncertain";

#if defined(NINFER_IO_URING_TESTING)
std::atomic<bool> fail_next_submitted_batch{false};
std::atomic<bool> fail_submit_followup{false};
std::atomic<bool> fail_publication_marker_fsync{false};
std::atomic<int> fail_publication_fsyncs{0};
#endif

constexpr long kExt4SuperMagic    = 0xef53;
constexpr long kXfsSuperMagic     = 0x58465342;
constexpr long kBtrfsSuperMagic   = 0x9123683e;
constexpr long kF2fsSuperMagic    = 0xf2f52010;
constexpr long kZfsSuperMagic     = 0x2fc12fc1;
constexpr long kWslFsSuperMagic   = 0x53464846;
constexpr long kPlan9SuperMagic   = 0x01021997;
constexpr long kFuseSuperMagic    = 0x65735546;
constexpr long kOverlaySuperMagic = 0x794c7630;

[[noreturn]] void throw_system_error(std::string_view operation, int error) {
    throw CheckpointContractError(std::string(operation) + ": " +
                                  std::error_code(error, std::generic_category()).message());
}

[[noreturn]] void throw_last_error(std::string_view operation) {
    throw_system_error(operation, errno);
}

int submit_io_uring(int ring_fd, unsigned submissions) {
#if defined(NINFER_IO_URING_TESTING)
    if (fail_submit_followup.exchange(false, std::memory_order_acq_rel)) {
        errno = EIO;
        return -1;
    }
    if (submissions > 1 && fail_next_submitted_batch.exchange(false, std::memory_order_acq_rel)) {
        const int partial = static_cast<int>(
            ::syscall(SYS_io_uring_enter, ring_fd, submissions - 1U, 0U, 0U, nullptr, 0U));
        if (partial > 0) { fail_submit_followup.store(true, std::memory_order_release); }
        return partial;
    }
#endif
    const int result =
        static_cast<int>(::syscall(SYS_io_uring_enter, ring_fd, submissions, 0U, 0U, nullptr, 0U));
    return result;
}

class UniqueFd {
public:
    UniqueFd() = default;

    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) { ::close(fd_); }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

struct RingRequest {
    std::uint8_t opcode       = 0;
    int fd                    = -1;
    std::uint64_t offset      = 0;
    void* address             = nullptr;
    std::uint32_t length      = 0;
    std::uint32_t fsync_flags = 0;
};

class NativeIoUring {
public:
    NativeIoUring() = default;

    ~NativeIoUring() { reset(); }

    NativeIoUring(const NativeIoUring&)            = delete;
    NativeIoUring& operator=(const NativeIoUring&) = delete;

    void initialize() {
        if (ring_fd_) { throw CheckpointContractError("io_uring is already initialized"); }

        io_uring_params params{};
        const int fd =
            static_cast<int>(::syscall(SYS_io_uring_setup, kRequestedRingEntries, &params));
        if (fd < 0) { throw_last_error("io_uring_setup"); }
        ring_fd_.reset(fd);

        if (params.sq_entries == 0 || params.cq_entries < params.sq_entries) {
            reset();
            throw CheckpointContractError("io_uring returned invalid queue dimensions");
        }

        sq_ring_bytes_ = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
        cq_ring_bytes_ = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        sqes_bytes_    = params.sq_entries * sizeof(io_uring_sqe);

        if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0U) {
            sq_ring_bytes_ = std::max(sq_ring_bytes_, cq_ring_bytes_);
            sq_ring_       = ::mmap(nullptr, sq_ring_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED,
                                    ring_fd_.get(), IORING_OFF_SQ_RING);
            if (sq_ring_ == MAP_FAILED) {
                sq_ring_        = nullptr;
                const int error = errno;
                reset();
                throw_system_error("mmap io_uring shared rings", error);
            }
            cq_ring_       = sq_ring_;
            cq_ring_bytes_ = sq_ring_bytes_;
            single_mmap_   = true;
        } else {
            sq_ring_ = ::mmap(nullptr, sq_ring_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED,
                              ring_fd_.get(), IORING_OFF_SQ_RING);
            if (sq_ring_ == MAP_FAILED) {
                sq_ring_        = nullptr;
                const int error = errno;
                reset();
                throw_system_error("mmap io_uring submission ring", error);
            }
            cq_ring_ = ::mmap(nullptr, cq_ring_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED,
                              ring_fd_.get(), IORING_OFF_CQ_RING);
            if (cq_ring_ == MAP_FAILED) {
                cq_ring_        = nullptr;
                const int error = errno;
                reset();
                throw_system_error("mmap io_uring completion ring", error);
            }
        }

        sqes_map_ = ::mmap(nullptr, sqes_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd_.get(),
                           IORING_OFF_SQES);
        if (sqes_map_ == MAP_FAILED) {
            sqes_map_       = nullptr;
            const int error = errno;
            reset();
            throw_system_error("mmap io_uring submission entries", error);
        }

        auto* sq_base = static_cast<std::byte*>(sq_ring_);
        sq_head_      = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.head);
        sq_tail_      = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.tail);
        sq_mask_      = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.ring_mask);
        sq_entries_   = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.ring_entries);
        sq_array_     = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.array);
        sqes_         = static_cast<io_uring_sqe*>(sqes_map_);

        auto* cq_base = static_cast<std::byte*>(cq_ring_);
        cq_head_      = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.head);
        cq_tail_      = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.tail);
        cq_mask_      = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.ring_mask);
        cq_entries_   = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.ring_entries);
        cqes_         = reinterpret_cast<io_uring_cqe*>(cq_base + params.cq_off.cqes);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return sq_entries_ == nullptr ? 0 : *sq_entries_;
    }

    [[nodiscard]] bool available() const noexcept { return static_cast<bool>(ring_fd_); }

    void execute(std::span<const RingRequest> requests, std::span<std::int32_t> results) {
        try {
            execute_unchecked(requests, results);
        } catch (...) {
            // Closing the ring cancels and joins every request owned by this io_uring context
            // before caller-owned bounce buffers and descriptors unwind. The ring is poisoned and
            // deliberately cannot be reused after an attribution or syscall failure.
            reset();
            throw;
        }
    }

    void execute_unchecked(std::span<const RingRequest> requests, std::span<std::int32_t> results) {
        if (requests.empty() || requests.size() != results.size()) {
            throw CheckpointContractError("io_uring request/result batch is invalid");
        }
        if (requests.size() > capacity()) {
            throw CheckpointContractError("io_uring request batch exceeds the queue capacity");
        }
        if (next_batch_id_ == std::numeric_limits<std::uint32_t>::max()) {
            throw CheckpointContractError("io_uring completion batch identity is exhausted");
        }
        const std::uint32_t batch_id  = ++next_batch_id_;
        const std::uint64_t batch_tag = static_cast<std::uint64_t>(batch_id) << 32U;

        const std::uint32_t head = std::atomic_ref(*sq_head_).load(std::memory_order_acquire);
        const std::uint32_t tail = std::atomic_ref(*sq_tail_).load(std::memory_order_relaxed);
        if (requests.size() > static_cast<std::size_t>(*sq_entries_ - (tail - head))) {
            throw CheckpointContractError("io_uring submission queue still has outstanding work");
        }

        for (std::size_t index = 0; index < requests.size(); ++index) {
            const std::uint32_t sqe_index = (tail + static_cast<std::uint32_t>(index)) & *sq_mask_;
            io_uring_sqe& sqe             = sqes_[sqe_index];
            std::memset(&sqe, 0, sizeof(sqe));
            const RingRequest& request = requests[index];
            sqe.opcode                 = request.opcode;
            sqe.fd                     = request.fd;
            sqe.off                    = request.offset;
            sqe.addr                   = reinterpret_cast<std::uint64_t>(request.address);
            sqe.len                    = request.length;
            sqe.fsync_flags            = request.fsync_flags;
            sqe.user_data              = batch_tag | (index + 1U);
            sq_array_[(tail + static_cast<std::uint32_t>(index)) & *sq_mask_] = sqe_index;
            results[index] = std::numeric_limits<std::int32_t>::min();
        }
        std::atomic_ref(*sq_tail_).store(tail + static_cast<std::uint32_t>(requests.size()),
                                         std::memory_order_release);

        std::size_t submitted = 0;
        while (submitted < requests.size()) {
            const unsigned remaining = static_cast<unsigned>(requests.size() - submitted);
            const int result         = submit_io_uring(ring_fd_.get(), remaining);
            if (result < 0) {
                if (errno == EINTR) { continue; }
                throw_last_error("io_uring_enter submit");
            }
            if (result == 0) {
                throw CheckpointContractError("io_uring submitted no queued operations");
            }
            submitted += static_cast<std::size_t>(result);
        }

        std::array<bool, kRequestedRingEntries> observed{};
        std::size_t completed = 0;
        while (completed < requests.size()) {
            std::uint32_t cq_head = std::atomic_ref(*cq_head_).load(std::memory_order_relaxed);
            const std::uint32_t cq_tail =
                std::atomic_ref(*cq_tail_).load(std::memory_order_acquire);
            while (cq_head != cq_tail && completed < requests.size()) {
                const io_uring_cqe& cqe = cqes_[cq_head & *cq_mask_];
                if ((cqe.user_data >> 32U) != batch_id || (cqe.user_data & 0xffffffffULL) == 0 ||
                    (cqe.user_data & 0xffffffffULL) > requests.size()) {
                    throw CheckpointContractError("io_uring returned an unknown completion key");
                }
                const std::size_t index =
                    static_cast<std::size_t>((cqe.user_data & 0xffffffffULL) - 1U);
                if (observed[index]) {
                    throw CheckpointContractError("io_uring returned a duplicate completion");
                }
                observed[index] = true;
                results[index]  = cqe.res;
                ++completed;
                ++cq_head;
            }
            std::atomic_ref(*cq_head_).store(cq_head, std::memory_order_release);
            if (completed == requests.size()) { break; }

            const int result = static_cast<int>(::syscall(SYS_io_uring_enter, ring_fd_.get(), 0U,
                                                          1U, IORING_ENTER_GETEVENTS, nullptr, 0U));
            if (result < 0 && errno != EINTR) { throw_last_error("io_uring_enter wait"); }
        }
    }

    [[nodiscard]] std::int32_t execute_one(const RingRequest& request) {
        std::array<RingRequest, 1> requests{request};
        std::array<std::int32_t, 1> results{};
        execute(requests, results);
        return results[0];
    }

    void fsync(int fd, std::string_view context) {
#if defined(NINFER_IO_URING_TESTING)
        if (context == "io_uring fsync checkpoint publication marker" &&
            fail_publication_marker_fsync.exchange(false, std::memory_order_acq_rel)) {
            throw CheckpointContractError("injected checkpoint publication marker failure");
        }
        if ((context == "io_uring fsync checkpoint manifest publication" ||
             context == "io_uring fsync checkpoint manifest rollback") &&
            fail_publication_fsyncs.load(std::memory_order_acquire) > 0) {
            fail_publication_fsyncs.fetch_sub(1, std::memory_order_acq_rel);
            throw CheckpointContractError("injected checkpoint publication fsync failure");
        }
#endif
        const std::int32_t result = execute_one(RingRequest{IORING_OP_FSYNC, fd, 0, nullptr, 0, 0});
        require_result(result, 0, context);
    }

    static void require_result(std::int32_t result, std::uint64_t expected,
                               std::string_view context) {
        if (result < 0) { throw_system_error(context, -result); }
        if (static_cast<std::uint64_t>(result) != expected) {
            throw CheckpointContractError(std::string(context) + " completed " +
                                          std::to_string(result) + " of " +
                                          std::to_string(expected) + " bytes");
        }
    }

private:
    void reset() noexcept {
        // Closing first synchronously tears down/cancels the io_uring context while every submitted
        // request still references live caller storage. Mappings are released only afterward.
        ring_fd_.reset();
        if (sqes_map_ != nullptr) { ::munmap(sqes_map_, sqes_bytes_); }
        if (single_mmap_) {
            if (sq_ring_ != nullptr) { ::munmap(sq_ring_, sq_ring_bytes_); }
        } else {
            if (cq_ring_ != nullptr) { ::munmap(cq_ring_, cq_ring_bytes_); }
            if (sq_ring_ != nullptr) { ::munmap(sq_ring_, sq_ring_bytes_); }
        }
        sqes_map_      = nullptr;
        sq_ring_       = nullptr;
        cq_ring_       = nullptr;
        sq_ring_bytes_ = 0;
        cq_ring_bytes_ = 0;
        sqes_bytes_    = 0;
        single_mmap_   = false;
        sq_head_       = nullptr;
        sq_tail_       = nullptr;
        sq_mask_       = nullptr;
        sq_entries_    = nullptr;
        sq_array_      = nullptr;
        sqes_          = nullptr;
        cq_head_       = nullptr;
        cq_tail_       = nullptr;
        cq_mask_       = nullptr;
        cq_entries_    = nullptr;
        cqes_          = nullptr;
        next_batch_id_ = 0;
    }

    UniqueFd ring_fd_;
    void* sq_ring_             = nullptr;
    void* cq_ring_             = nullptr;
    void* sqes_map_            = nullptr;
    std::size_t sq_ring_bytes_ = 0;
    std::size_t cq_ring_bytes_ = 0;
    std::size_t sqes_bytes_    = 0;
    bool single_mmap_          = false;

    std::uint32_t* sq_head_    = nullptr;
    std::uint32_t* sq_tail_    = nullptr;
    std::uint32_t* sq_mask_    = nullptr;
    std::uint32_t* sq_entries_ = nullptr;
    std::uint32_t* sq_array_   = nullptr;
    io_uring_sqe* sqes_        = nullptr;

    std::uint32_t* cq_head_      = nullptr;
    std::uint32_t* cq_tail_      = nullptr;
    std::uint32_t* cq_mask_      = nullptr;
    std::uint32_t* cq_entries_   = nullptr;
    io_uring_cqe* cqes_          = nullptr;
    std::uint32_t next_batch_id_ = 0;
};

class AlignedBuffer {
public:
    AlignedBuffer(std::size_t alignment, std::size_t bytes) : bytes_(bytes) {
        void* allocation = nullptr;
        const int result = ::posix_memalign(&allocation, alignment, bytes);
        if (result != 0) { throw_system_error("allocate aligned direct-I/O buffer", result); }
        data_ = static_cast<std::byte*>(allocation);
    }

    ~AlignedBuffer() { std::free(data_); }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            std::free(data_);
            data_  = std::exchange(other.data_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    [[nodiscard]] std::byte* data() noexcept { return data_; }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_; }

private:
    std::byte* data_   = nullptr;
    std::size_t bytes_ = 0;
};

struct DirectIoAlignment {
    std::size_t memory = 0;
    std::size_t offset = 0;
};

struct EncodedManifest {
    std::array<std::byte, kMaximumManifestBytes> bytes{};
    std::size_t size = 0;
};

template <typename Integer>
void append_integer(EncodedManifest& encoded, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        const unsigned shift          = static_cast<unsigned>((sizeof(Integer) - 1U - index) * 8U);
        encoded.bytes[encoded.size++] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void append_digest(EncodedManifest& encoded, const CheckpointDigest& digest) {
    for (const std::uint8_t byte : digest) { encoded.bytes[encoded.size++] = std::byte{byte}; }
}

template <typename Integer>
Integer take_integer(std::span<const std::byte> encoded, std::size_t& cursor) {
    static_assert(std::is_unsigned_v<Integer>);
    if (encoded.size() - cursor < sizeof(Integer)) {
        throw CheckpointContractError("checkpoint manifest is truncated");
    }
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value =
            static_cast<Integer>((value << 8U) | std::to_integer<std::uint8_t>(encoded[cursor++]));
    }
    return value;
}

CheckpointDigest take_digest(std::span<const std::byte> encoded, std::size_t& cursor) {
    if (encoded.size() - cursor < CheckpointDigest{}.size()) {
        throw CheckpointContractError("checkpoint manifest digest is truncated");
    }
    CheckpointDigest digest{};
    for (std::uint8_t& byte : digest) { byte = std::to_integer<std::uint8_t>(encoded[cursor++]); }
    return digest;
}

void validate_manifest(const CheckpointManifestV1& manifest,
                       const IoUringCheckpointLimits& limits) {
    if (limits.max_payload_bytes == 0 || limits.max_total_payload_bytes == 0) {
        throw CheckpointContractError("checkpoint allocation limits must be nonzero");
    }
    if (manifest.magic != kCheckpointMagic || manifest.schema_version != kCheckpointSchemaVersion ||
        manifest.journal_version != kCheckpointJournalVersion || manifest.generation == 0 ||
        manifest.identity.token_count == 0 || manifest.identity.context_capacity == 0 ||
        manifest.identity.token_count > manifest.identity.context_capacity ||
        manifest.payloads.empty() || manifest.payloads.size() > kPayloadKindCount) {
        throw CheckpointContractError("checkpoint manifest structure is invalid");
    }

    std::array<bool, kPayloadKindCount> observed{};
    std::uint64_t total = 0;
    for (const CheckpointPayloadDescriptor& descriptor : manifest.payloads) {
        const std::size_t kind = static_cast<std::size_t>(descriptor.kind);
        if (kind >= observed.size() || observed[kind] || descriptor.bytes == 0) {
            throw CheckpointContractError("checkpoint payload descriptors are invalid");
        }
        observed[kind] = true;
        if (descriptor.bytes > limits.max_payload_bytes ||
            descriptor.bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            descriptor.bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            throw CheckpointContractError(
                "checkpoint payload exceeds the configured allocation bound");
        }
        if (descriptor.bytes > limits.max_total_payload_bytes - total) {
            throw CheckpointContractError(
                "checkpoint payload set exceeds the configured allocation bound");
        }
        total += descriptor.bytes;
    }
}

EncodedManifest encode_manifest(const CheckpointManifestV1& manifest,
                                const IoUringCheckpointLimits& limits) {
    validate_manifest(manifest, limits);
    EncodedManifest encoded;
    append_integer(encoded, manifest.magic);
    append_integer(encoded, manifest.schema_version);
    append_integer(encoded, manifest.journal_version);
    append_integer(encoded, manifest.generation);
    append_digest(encoded, manifest.identity.model);
    append_digest(encoded, manifest.identity.runtime_source);
    append_digest(encoded, manifest.identity.deployment_profile);
    append_digest(encoded, manifest.identity.layout);
    append_integer(encoded, manifest.identity.token_count);
    append_integer(encoded, manifest.identity.context_capacity);
    append_integer(encoded, static_cast<std::uint32_t>(manifest.payloads.size()));
    for (const CheckpointPayloadDescriptor& descriptor : manifest.payloads) {
        append_integer(encoded, static_cast<std::uint8_t>(descriptor.kind));
        append_integer(encoded, descriptor.bytes);
        append_digest(encoded, descriptor.sha256);
    }
    if (sha256(std::span(encoded.bytes).first(encoded.size)) !=
        checkpoint_manifest_sha256(manifest)) {
        throw CheckpointContractError("checkpoint manifest encoding is not canonical");
    }
    return encoded;
}

CheckpointManifestV1 decode_manifest(std::span<const std::byte> encoded,
                                     const IoUringCheckpointLimits& limits) {
    if (encoded.size() < kManifestBaseBytes + kDescriptorBytes ||
        encoded.size() > kMaximumManifestBytes) {
        throw CheckpointContractError("checkpoint manifest length is invalid");
    }

    std::size_t cursor = 0;
    CheckpointManifestV1 manifest;
    manifest.magic                       = take_integer<std::uint64_t>(encoded, cursor);
    manifest.schema_version              = take_integer<std::uint32_t>(encoded, cursor);
    manifest.journal_version             = take_integer<std::uint32_t>(encoded, cursor);
    manifest.generation                  = take_integer<std::uint64_t>(encoded, cursor);
    manifest.identity.model              = take_digest(encoded, cursor);
    manifest.identity.runtime_source     = take_digest(encoded, cursor);
    manifest.identity.deployment_profile = take_digest(encoded, cursor);
    manifest.identity.layout             = take_digest(encoded, cursor);
    manifest.identity.token_count        = take_integer<std::uint32_t>(encoded, cursor);
    manifest.identity.context_capacity   = take_integer<std::uint32_t>(encoded, cursor);
    const std::uint32_t payload_count    = take_integer<std::uint32_t>(encoded, cursor);
    if (payload_count == 0 || payload_count > kPayloadKindCount ||
        encoded.size() != kManifestBaseBytes + payload_count * kDescriptorBytes) {
        throw CheckpointContractError("checkpoint manifest payload count is invalid");
    }

    manifest.payloads.reserve(payload_count);
    for (std::uint32_t index = 0; index < payload_count; ++index) {
        CheckpointPayloadDescriptor descriptor;
        descriptor.kind =
            static_cast<CheckpointPayloadKind>(take_integer<std::uint8_t>(encoded, cursor));
        descriptor.bytes  = take_integer<std::uint64_t>(encoded, cursor);
        descriptor.sha256 = take_digest(encoded, cursor);
        manifest.payloads.push_back(descriptor);
    }
    if (cursor != encoded.size()) {
        throw CheckpointContractError("checkpoint manifest has trailing bytes");
    }
    validate_manifest(manifest, limits);
    return manifest;
}

[[nodiscard]] bool stage_keys_equal(const CheckpointStageKey& left,
                                    const CheckpointStageKey& right) noexcept {
    return left.journal_version == right.journal_version && left.generation == right.generation &&
           left.manifest_sha256 == right.manifest_sha256;
}

void validate_stage_key(const CheckpointManifestV1& manifest, const CheckpointStageKey& key) {
    if (key.journal_version != kCheckpointJournalVersion ||
        key.journal_version != manifest.journal_version || key.generation != manifest.generation ||
        key.manifest_sha256 != checkpoint_manifest_sha256(manifest)) {
        throw CheckpointContractError("checkpoint stage key does not identify the manifest");
    }
}

void validate_payload_shapes(const CheckpointManifestV1& manifest,
                             std::span<const CheckpointPayload> payloads) {
    if (payloads.size() != manifest.payloads.size()) {
        throw CheckpointContractError("checkpoint payload count differs from the manifest");
    }
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const CheckpointPayloadDescriptor& descriptor = manifest.payloads[index];
        const CheckpointPayload& payload              = payloads[index];
        if (payload.kind != descriptor.kind || payload.bytes.size() != descriptor.bytes) {
            throw CheckpointContractError("checkpoint payload shape differs from the manifest");
        }
    }
}

std::string generation_name(std::uint64_t generation, const CheckpointDigest& digest) {
    std::string decimal = std::to_string(generation);
    if (decimal.size() < 20) { decimal.insert(decimal.begin(), 20 - decimal.size(), '0'); }
    return "generation-" + decimal + "-" + sha256_hex(digest);
}

std::string stage_name(const CheckpointStageKey& key) {
    std::string decimal = std::to_string(key.generation);
    if (decimal.size() < 20) { decimal.insert(decimal.begin(), 20 - decimal.size(), '0'); }
    return ".stage-" + decimal + "-" + sha256_hex(key.manifest_sha256);
}

std::string publish_name(const CheckpointStageKey& key) {
    return ".publish-" + sha256_hex(key.manifest_sha256);
}

std::string payload_name(CheckpointPayloadKind kind) {
    return "payload-" + std::to_string(static_cast<unsigned>(kind));
}

[[nodiscard]] bool decimal_digits(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](char byte) { return byte >= '0' && byte <= '9'; });
}

[[nodiscard]] bool lower_hex(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

[[nodiscard]] bool owned_generation_name(std::string_view name) noexcept {
    constexpr std::string_view prefix = "generation-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 20 + 1 + 64) { return false; }
    const std::string_view suffix = name.substr(prefix.size());
    return suffix[20] == '-' && decimal_digits(suffix.substr(0, 20)) &&
           lower_hex(suffix.substr(21));
}

[[nodiscard]] bool owned_stage_name(std::string_view name) noexcept {
    constexpr std::string_view prefix = ".stage-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 20 + 1 + 64) { return false; }
    const std::string_view suffix = name.substr(prefix.size());
    return suffix[20] == '-' && decimal_digits(suffix.substr(0, 20)) &&
           lower_hex(suffix.substr(21));
}

[[nodiscard]] bool owned_publish_name(std::string_view name) noexcept {
    constexpr std::string_view prefix = ".publish-";
    return name.starts_with(prefix) && name.size() == prefix.size() + 64 &&
           lower_hex(name.substr(prefix.size()));
}

[[nodiscard]] std::uint64_t round_up(std::uint64_t value, std::size_t alignment) {
    const std::uint64_t mask = alignment - 1U;
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
        throw CheckpointContractError("checkpoint direct-I/O length overflows");
    }
    return (value + mask) & ~mask;
}

LinuxCheckpointEnvironment detect_environment() noexcept {
    utsname system{};
    if (::uname(&system) != 0) { return LinuxCheckpointEnvironment::NativeLinux; }
    std::string release(system.release);
    std::transform(release.begin(), release.end(), release.begin(), [](unsigned char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    return release.find("microsoft") == std::string::npos ? LinuxCheckpointEnvironment::NativeLinux
                                                          : LinuxCheckpointEnvironment::Wsl;
}

UniqueFd open_root(const std::filesystem::path& root) {
    const int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { throw_last_error("open checkpoint root"); }
    return UniqueFd(fd);
}

void require_local_filesystem(int root_fd, LinuxCheckpointEnvironment environment) {
    struct statfs filesystem{};
    if (::fstatfs(root_fd, &filesystem) != 0) { throw_last_error("inspect checkpoint filesystem"); }
    const long type = static_cast<long>(filesystem.f_type);
    if (type == kExt4SuperMagic || type == kXfsSuperMagic || type == kBtrfsSuperMagic ||
        type == kF2fsSuperMagic || type == kZfsSuperMagic) {
        return;
    }
    if (environment == LinuxCheckpointEnvironment::Wsl &&
        (type == kWslFsSuperMagic || type == kPlan9SuperMagic || type == kFuseSuperMagic)) {
        throw CheckpointContractError(
            "WSL DrvFS/9p paths are unsupported; use the WSL ext4 filesystem backed by local NVMe");
    }
    if (type == kOverlaySuperMagic) {
        throw CheckpointContractError(
            "overlayfs is unsupported for durable checkpoint publication; use a local NVMe mount");
    }
    throw CheckpointContractError(
        "checkpoint root must use a supported local ext4, XFS, Btrfs, F2FS, or ZFS filesystem");
}

void rename_noreplace(int root_fd, std::string_view from, std::string_view to) {
    const std::string from_name(from);
    const std::string to_name(to);
    if (::syscall(SYS_renameat2, root_fd, from_name.c_str(), root_fd, to_name.c_str(),
                  RENAME_NOREPLACE) != 0) {
        throw_last_error("atomically publish checkpoint path");
    }
}

void rename_exchange(int root_fd, std::string_view left, std::string_view right) {
    const std::string left_name(left);
    const std::string right_name(right);
    if (::syscall(SYS_renameat2, root_fd, left_name.c_str(), root_fd, right_name.c_str(),
                  RENAME_EXCHANGE) != 0) {
        throw_last_error("atomically exchange checkpoint manifests");
    }
}

void unlink_if_exists(int directory_fd, std::string_view name, int flags = 0) {
    const std::string path(name);
    if (::unlinkat(directory_fd, path.c_str(), flags) != 0 && errno != ENOENT) {
        throw_last_error("remove checkpoint staging path");
    }
}

bool entry_exists(int directory_fd, std::string_view name) {
    const std::string path(name);
    struct stat status{};
    if (::fstatat(directory_fd, path.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) { return true; }
    if (errno == ENOENT) { return false; }
    throw_last_error("inspect checkpoint publication marker");
}

DirectIoAlignment verify_storage_capabilities(int root_fd, NativeIoUring& ring) {
    static std::atomic<std::uint64_t> sequence{0};
    const std::string nonce = std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
                              std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    const std::string direct_name = ".io-uring-probe-" + nonce + "-a";
    const std::string other_name  = ".io-uring-probe-" + nonce + "-b";

    UniqueFd direct;
    UniqueFd other;
    bool direct_exists = false;
    bool other_exists  = false;
    auto cleanup       = [&]() noexcept {
        direct.reset();
        other.reset();
        if (direct_exists) { ::unlinkat(root_fd, direct_name.c_str(), 0); }
        if (other_exists) { ::unlinkat(root_fd, other_name.c_str(), 0); }
        try {
            ring.fsync(root_fd, "fsync checkpoint root after capability probe cleanup");
        } catch (...) {}
    };

    try {
        const int direct_fd =
            ::openat(root_fd, direct_name.c_str(),
                     O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_DIRECT, 0600);
        if (direct_fd < 0) { throw_last_error("open O_DIRECT checkpoint capability probe"); }
        direct.reset(direct_fd);
        direct_exists = true;

        struct statx status{};
        if (::syscall(SYS_statx, direct.get(), "", AT_EMPTY_PATH, STATX_DIOALIGN, &status) != 0) {
            throw_last_error("statx checkpoint direct-I/O alignment");
        }
        if ((status.stx_mask & STATX_DIOALIGN) == 0 || status.stx_dio_mem_align == 0 ||
            status.stx_dio_offset_align == 0) {
            throw CheckpointContractError(
                "checkpoint filesystem does not report STATX_DIOALIGN capability");
        }

        DirectIoAlignment alignment{status.stx_dio_mem_align, status.stx_dio_offset_align};
        if (!std::has_single_bit(alignment.memory) || !std::has_single_bit(alignment.offset) ||
            alignment.memory > kMaximumDioAlignment || alignment.offset > kMaximumDioAlignment) {
            throw CheckpointContractError(
                "checkpoint filesystem reported unsupported O_DIRECT alignment");
        }
        const std::size_t allocation_alignment =
            std::max(alignment.memory, static_cast<std::size_t>(alignof(std::max_align_t)));
        const std::size_t io_bytes =
            std::max<std::size_t>(4096U, std::max(alignment.memory, alignment.offset));
        if (io_bytes > std::numeric_limits<std::uint32_t>::max()) {
            throw CheckpointContractError(
                "checkpoint direct-I/O alignment exceeds io_uring limits");
        }
        const std::size_t logical_bytes = io_bytes - 1U;
        AlignedBuffer buffer(allocation_alignment, io_bytes);
        for (std::size_t index = 0; index < logical_bytes; ++index) {
            buffer.data()[index] = static_cast<std::byte>((index * 131U + 17U) & 0xffU);
        }
        buffer.data()[logical_bytes] = std::byte{0};

        std::int32_t result =
            ring.execute_one(RingRequest{IORING_OP_WRITE, direct.get(), 0, buffer.data(),
                                         static_cast<std::uint32_t>(io_bytes), 0});
        NativeIoUring::require_result(result, io_bytes, "io_uring O_DIRECT capability write");
        if (::ftruncate(direct.get(), static_cast<off_t>(logical_bytes)) != 0) {
            throw_last_error("truncate checkpoint capability probe to logical length");
        }
        ring.fsync(direct.get(), "io_uring fsync checkpoint capability probe");
        ring.fsync(root_fd, "io_uring fsync checkpoint root capability probe");

        std::memset(buffer.data(), 0, buffer.size());
        result = ring.execute_one(RingRequest{IORING_OP_READ, direct.get(), 0, buffer.data(),
                                              static_cast<std::uint32_t>(io_bytes), 0});
        NativeIoUring::require_result(result, logical_bytes,
                                      "io_uring O_DIRECT logical-length capability read");
        for (std::size_t index = 0; index < logical_bytes; ++index) {
            const std::byte expected = static_cast<std::byte>((index * 131U + 17U) & 0xffU);
            if (buffer.data()[index] != expected) {
                throw CheckpointContractError("checkpoint O_DIRECT capability probe was corrupted");
            }
        }

        const int other_fd = ::openat(root_fd, other_name.c_str(),
                                      O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (other_fd < 0) { throw_last_error("create checkpoint rename capability probe"); }
        other.reset(other_fd);
        other_exists = true;
        std::byte marker{0x5a};
        result = ring.execute_one(RingRequest{IORING_OP_WRITE, other.get(), 0, &marker, 1, 0});
        NativeIoUring::require_result(result, 1, "io_uring checkpoint manifest capability write");
        ring.fsync(other.get(), "io_uring fsync checkpoint manifest capability probe");
        ring.fsync(root_fd, "io_uring fsync checkpoint rename capability entries");
        rename_exchange(root_fd, direct_name, other_name);
        rename_exchange(root_fd, direct_name, other_name);
        ring.fsync(root_fd, "io_uring fsync checkpoint rename-exchange capability probe");

        direct.reset();
        other.reset();
        unlink_if_exists(root_fd, direct_name);
        direct_exists = false;
        unlink_if_exists(root_fd, other_name);
        other_exists = false;
        ring.fsync(root_fd, "io_uring fsync checkpoint capability probe cleanup");
        return alignment;
    } catch (...) {
        cleanup();
        throw;
    }
}

struct StoredManifest {
    CheckpointManifestV1 manifest;
    CheckpointDigest digest{};
    EncodedManifest encoded;
};

std::optional<StoredManifest> read_manifest_file(NativeIoUring& ring, int directory_fd,
                                                 std::string_view name,
                                                 const IoUringCheckpointLimits& limits,
                                                 bool allow_missing) {
    const std::string path(name);
    const int raw_fd = ::openat(directory_fd, path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        if (allow_missing && errno == ENOENT) { return std::nullopt; }
        throw_last_error("open checkpoint manifest");
    }
    UniqueFd fd(raw_fd);

    struct stat status{};
    if (::fstat(fd.get(), &status) != 0) { throw_last_error("stat checkpoint manifest"); }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaximumManifestBytes ||
        static_cast<std::uint64_t>(status.st_size) < kManifestBaseBytes + kDescriptorBytes) {
        throw CheckpointContractError("checkpoint manifest file length is invalid");
    }

    EncodedManifest encoded;
    encoded.size = static_cast<std::size_t>(status.st_size);
    const std::int32_t result =
        ring.execute_one(RingRequest{IORING_OP_READ, fd.get(), 0, encoded.bytes.data(),
                                     static_cast<std::uint32_t>(encoded.size), 0});
    NativeIoUring::require_result(result, encoded.size, "io_uring checkpoint manifest read");

    const std::span<const std::byte> bytes = std::span(encoded.bytes).first(encoded.size);
    StoredManifest stored;
    stored.digest                   = sha256(bytes);
    stored.manifest                 = decode_manifest(bytes, limits);
    stored.encoded                  = encoded;
    const EncodedManifest canonical = encode_manifest(stored.manifest, limits);
    if (canonical.size != encoded.size ||
        !std::equal(canonical.bytes.begin(), canonical.bytes.begin() + canonical.size,
                    encoded.bytes.begin()) ||
        stored.digest != checkpoint_manifest_sha256(stored.manifest)) {
        throw CheckpointContractError("checkpoint manifest file is not canonical");
    }
    return stored;
}

void write_buffered_file(NativeIoUring& ring, int directory_fd, std::string_view name,
                         const EncodedManifest& encoded) {
    const std::string path(name);
    const int raw_fd = ::openat(directory_fd, path.c_str(),
                                O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (raw_fd < 0) { throw_last_error("create checkpoint manifest staging file"); }
    UniqueFd fd(raw_fd);
    const std::int32_t result = ring.execute_one(
        RingRequest{IORING_OP_WRITE, fd.get(), 0, const_cast<std::byte*>(encoded.bytes.data()),
                    static_cast<std::uint32_t>(encoded.size), 0});
    NativeIoUring::require_result(result, encoded.size, "io_uring checkpoint manifest write");
    ring.fsync(fd.get(), "io_uring fsync checkpoint manifest");
}

struct DirectWriteFile {
    int fd = -1;
    std::span<const std::byte> bytes;
};

struct DirectReadFile {
    int fd                                        = -1;
    CheckpointPayload* payload                    = nullptr;
    const CheckpointPayloadDescriptor* descriptor = nullptr;
};

std::size_t batch_buffer_count(std::span<const std::uint64_t> lengths) {
    std::size_t chunks = 0;
    for (const std::uint64_t length : lengths) {
        const std::uint64_t count = (length + kDirectChunkBytes - 1U) / kDirectChunkBytes;
        chunks += static_cast<std::size_t>(std::min<std::uint64_t>(count, kIoQueueDepth));
        if (chunks >= kIoQueueDepth) { return kIoQueueDepth; }
    }
    return std::max<std::size_t>(1, chunks);
}

std::vector<AlignedBuffer> make_batch_buffers(std::size_t count, DirectIoAlignment alignment,
                                              std::size_t bytes) {
    std::vector<AlignedBuffer> buffers;
    buffers.reserve(count);
    const std::size_t allocation_alignment =
        std::max(alignment.memory, static_cast<std::size_t>(alignof(std::max_align_t)));
    for (std::size_t index = 0; index < count; ++index) {
        buffers.emplace_back(allocation_alignment, bytes);
    }
    return buffers;
}

void direct_write_payloads(NativeIoUring& ring, std::span<const DirectWriteFile> files,
                           DirectIoAlignment alignment) {
    std::array<std::uint64_t, kPayloadKindCount> lengths{};
    for (std::size_t index = 0; index < files.size(); ++index) {
        lengths[index] = files[index].bytes.size();
    }
    const std::span<const std::uint64_t> active_lengths = std::span(lengths).first(files.size());
    const std::size_t depth                             = batch_buffer_count(active_lengths);
    const std::uint64_t largest = *std::max_element(active_lengths.begin(), active_lengths.end());
    const std::size_t buffer_bytes = static_cast<std::size_t>(
        round_up(std::min<std::uint64_t>(largest, kDirectChunkBytes), alignment.offset));
    std::vector<AlignedBuffer> buffers = make_batch_buffers(depth, alignment, buffer_bytes);
    std::array<std::uint64_t, kPayloadKindCount> offsets{};
    std::size_t cursor = 0;

    struct Work {
        std::size_t file      = 0;
        std::uint64_t offset  = 0;
        std::size_t logical   = 0;
        std::size_t submitted = 0;
    };

    std::array<RingRequest, kIoQueueDepth> requests{};
    std::array<std::int32_t, kIoQueueDepth> results{};
    std::array<Work, kIoQueueDepth> work{};

    for (;;) {
        std::size_t count = 0;
        while (count < depth) {
            std::optional<std::size_t> selected;
            for (std::size_t checked = 0; checked < files.size(); ++checked) {
                const std::size_t candidate = cursor++ % files.size();
                if (offsets[candidate] < files[candidate].bytes.size()) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected.has_value()) { break; }

            const std::size_t file        = *selected;
            const std::uint64_t remaining = files[file].bytes.size() - offsets[file];
            const std::size_t logical =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kDirectChunkBytes));
            const std::size_t submitted =
                static_cast<std::size_t>(round_up(logical, alignment.offset));
            if (submitted > buffers[count].size()) {
                throw CheckpointContractError(
                    "checkpoint direct-I/O chunk exceeds its aligned buffer");
            }
            std::memcpy(buffers[count].data(), files[file].bytes.data() + offsets[file], logical);
            if (submitted > logical) {
                std::memset(buffers[count].data() + logical, 0, submitted - logical);
            }
            work[count]     = Work{file, offsets[file], logical, submitted};
            requests[count] = RingRequest{IORING_OP_WRITE,
                                          files[file].fd,
                                          offsets[file],
                                          buffers[count].data(),
                                          static_cast<std::uint32_t>(submitted),
                                          0};
            offsets[file] += logical;
            ++count;
        }
        if (count == 0) { break; }
        ring.execute(std::span(requests).first(count), std::span(results).first(count));
        for (std::size_t index = 0; index < count; ++index) {
            NativeIoUring::require_result(results[index], work[index].submitted,
                                          "io_uring O_DIRECT checkpoint payload write");
        }
    }

    for (const DirectWriteFile& file : files) {
        if (::ftruncate(file.fd, static_cast<off_t>(file.bytes.size())) != 0) {
            throw_last_error("truncate checkpoint payload to its logical length");
        }
    }
}

void direct_read_payloads(NativeIoUring& ring, std::span<const DirectReadFile> files,
                          DirectIoAlignment alignment) {
    std::array<std::uint64_t, kPayloadKindCount> lengths{};
    for (std::size_t index = 0; index < files.size(); ++index) {
        lengths[index] = files[index].descriptor->bytes;
    }
    const std::span<const std::uint64_t> active_lengths = std::span(lengths).first(files.size());
    const std::size_t depth                             = batch_buffer_count(active_lengths);
    const std::uint64_t largest = *std::max_element(active_lengths.begin(), active_lengths.end());
    const std::size_t buffer_bytes = static_cast<std::size_t>(
        round_up(std::min<std::uint64_t>(largest, kDirectChunkBytes), alignment.offset));
    std::vector<AlignedBuffer> buffers = make_batch_buffers(depth, alignment, buffer_bytes);
    std::array<std::uint64_t, kPayloadKindCount> offsets{};
    std::array<Sha256, kPayloadKindCount> hashers{};
    std::size_t cursor = 0;

    struct Work {
        std::size_t file      = 0;
        std::uint64_t offset  = 0;
        std::size_t logical   = 0;
        std::size_t requested = 0;
    };

    std::array<RingRequest, kIoQueueDepth> requests{};
    std::array<std::int32_t, kIoQueueDepth> results{};
    std::array<Work, kIoQueueDepth> work{};

    for (;;) {
        std::size_t count = 0;
        while (count < depth) {
            std::optional<std::size_t> selected;
            for (std::size_t checked = 0; checked < files.size(); ++checked) {
                const std::size_t candidate = cursor++ % files.size();
                if (offsets[candidate] < files[candidate].descriptor->bytes) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected.has_value()) { break; }

            const std::size_t file        = *selected;
            const std::uint64_t remaining = files[file].descriptor->bytes - offsets[file];
            const std::size_t logical =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kDirectChunkBytes));
            const std::size_t requested =
                static_cast<std::size_t>(round_up(logical, alignment.offset));
            work[count]     = Work{file, offsets[file], logical, requested};
            requests[count] = RingRequest{IORING_OP_READ,
                                          files[file].fd,
                                          offsets[file],
                                          buffers[count].data(),
                                          static_cast<std::uint32_t>(requested),
                                          0};
            offsets[file] += logical;
            ++count;
        }
        if (count == 0) { break; }
        ring.execute(std::span(requests).first(count), std::span(results).first(count));
        for (std::size_t index = 0; index < count; ++index) {
            NativeIoUring::require_result(results[index], work[index].logical,
                                          "io_uring O_DIRECT checkpoint payload read");
            DirectReadFile file = files[work[index].file];
            std::memcpy(file.payload->bytes.data() + work[index].offset, buffers[index].data(),
                        work[index].logical);
            hashers[work[index].file].update(
                std::span<const std::byte>(buffers[index].data(), work[index].logical));
        }
    }

    for (std::size_t index = 0; index < files.size(); ++index) {
        if (hashers[index].finish() != files[index].descriptor->sha256) {
            throw CheckpointContractError(
                "checkpoint payload digest differs from the trusted manifest");
        }
    }
}

} // namespace

#if defined(NINFER_IO_URING_TESTING)
void io_uring_checkpoint_test_fail_next_submitted_batch() noexcept {
    fail_next_submitted_batch.store(true, std::memory_order_release);
}

void io_uring_checkpoint_test_fail_publication_and_rollback_fsync() noexcept {
    fail_publication_fsyncs.store(2, std::memory_order_release);
}

void io_uring_checkpoint_test_fail_publication_marker_fsync() noexcept {
    fail_publication_marker_fsync.store(true, std::memory_order_release);
}
#endif

class IoUringCheckpointBackend::Impl {
public:
    Impl(std::filesystem::path root, IoUringCheckpointLimits limits)
        : root_path_(std::move(root)), limits_(limits), environment_(detect_environment()),
          root_fd_(open_root(root_path_)) {
        if (limits_.lock_timeout_ms == 0) {
            throw CheckpointContractError("checkpoint lock timeout must be nonzero");
        }
        require_local_filesystem(root_fd_.get(), environment_);
        ring_.initialize();

        const int lock_fd = ::openat(root_fd_.get(), ".checkpoint.lock",
                                     O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd < 0) { throw_last_error("open checkpoint single-flight lock"); }
        lock_fd_.reset(lock_fd);
        struct stat lock_status{};
        if (::fstat(lock_fd_.get(), &lock_status) != 0) {
            throw_last_error("stat checkpoint single-flight lock");
        }
        if (!S_ISREG(lock_status.st_mode)) {
            throw CheckpointContractError("checkpoint single-flight lock is not a regular file");
        }

        acquire_lock(LOCK_EX);
        try {
            ensure_no_uncertain_publication();
            alignment_ = verify_storage_capabilities(root_fd_.get(), ring_);
            const std::optional<StoredManifest> current = read_current_manifest();
            cleanup_orphans(current);
            committed_generation_.store(current.has_value() ? current->manifest.generation : 0,
                                        std::memory_order_release);
            release_lock_noexcept();
        } catch (...) {
            release_lock_noexcept();
            throw;
        }
    }

    ~Impl() {
        std::lock_guard lock(mutex_);
        if (active_.has_value() && !active_->publication_uncertain) {
            remove_known_directory_noexcept(active_->path_name);
            ::unlinkat(root_fd_.get(), active_->publish_path.c_str(), 0);
            try {
                ring_.fsync(root_fd_.get(), "fsync checkpoint root during backend teardown");
            } catch (...) {}
        }
        active_.reset();
        release_lock_noexcept();
    }

    [[nodiscard]] std::uint64_t committed_generation() noexcept {
        bool acquired = false;
        try {
            std::lock_guard lock(mutex_);
            if (active_.has_value() || poisoned_) {
                return committed_generation_.load(std::memory_order_acquire);
            }
            acquire_lock(LOCK_SH);
            acquired = true;
            ensure_no_uncertain_publication();
            const std::optional<StoredManifest> current = read_current_manifest();
            const std::uint64_t generation = current.has_value() ? current->manifest.generation : 0;
            committed_generation_.store(generation, std::memory_order_release);
            release_lock_noexcept();
            return generation;
        } catch (...) {
            if (acquired) { release_lock_noexcept(); }
            return committed_generation_.load(std::memory_order_acquire);
        }
    }

    void stage(const CheckpointManifestV1& manifest, std::span<const CheckpointPayload> payloads,
               const CheckpointStageKey& key) {
        validate_manifest(manifest, limits_);
        validate_stage_key(manifest, key);
        validate_payload_shapes(manifest, payloads);
        const EncodedManifest encoded = encode_manifest(manifest, limits_);

        std::lock_guard lock(mutex_);
        ensure_ready();
        if (active_.has_value()) {
            throw CheckpointContractError("checkpoint backend already has an active transaction");
        }

        acquire_lock(LOCK_EX);
        try {
            ensure_no_uncertain_publication();
            const std::optional<StoredManifest> current = read_current_manifest();
            const std::uint64_t current_generation =
                current.has_value() ? current->manifest.generation : 0;
            committed_generation_.store(current_generation, std::memory_order_release);
            if (manifest.generation <= current_generation) {
                throw CheckpointContractError("checkpoint generation is stale");
            }
            cleanup_orphans(current);

            ActiveStage prepared;
            prepared.key          = key;
            prepared.manifest     = manifest;
            prepared.encoded      = encoded;
            prepared.path_name    = stage_name(key);
            prepared.final_name   = generation_name(key.generation, key.manifest_sha256);
            prepared.publish_path = publish_name(key);
            if (current.has_value()) {
                prepared.old_generation =
                    generation_name(current->manifest.generation, current->digest);
            }

            active_ = std::move(prepared);
            if (::mkdirat(root_fd_.get(), active_->path_name.c_str(), 0700) != 0) {
                throw_last_error("create checkpoint staging generation");
            }

            const int stage_fd = ::openat(root_fd_.get(), active_->path_name.c_str(),
                                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (stage_fd < 0) { throw_last_error("open checkpoint staging generation"); }
            UniqueFd directory(stage_fd);

            std::vector<UniqueFd> payload_fds;
            std::vector<DirectWriteFile> writes;
            payload_fds.reserve(payloads.size());
            writes.reserve(payloads.size());
            for (const CheckpointPayload& payload : payloads) {
                const std::string name = payload_name(payload.kind);
                const int fd =
                    ::openat(directory.get(), name.c_str(),
                             O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECT, 0600);
                if (fd < 0) { throw_last_error("create O_DIRECT checkpoint payload"); }
                payload_fds.emplace_back(fd);
                writes.push_back(DirectWriteFile{fd, payload.bytes});
            }

            direct_write_payloads(ring_, writes, alignment_);
            std::array<RingRequest, kPayloadKindCount> fsync_requests{};
            std::array<std::int32_t, kPayloadKindCount> fsync_results{};
            for (std::size_t index = 0; index < payload_fds.size(); ++index) {
                fsync_requests[index] =
                    RingRequest{IORING_OP_FSYNC, payload_fds[index].get(), 0, nullptr, 0, 0};
            }
            ring_.execute(std::span(fsync_requests).first(payload_fds.size()),
                          std::span(fsync_results).first(payload_fds.size()));
            for (std::size_t index = 0; index < payload_fds.size(); ++index) {
                NativeIoUring::require_result(fsync_results[index], 0,
                                              "io_uring fsync checkpoint payload");
            }
            payload_fds.clear();

            write_buffered_file(ring_, directory.get(), "manifest", active_->encoded);
            ring_.fsync(directory.get(), "io_uring fsync checkpoint staging generation");
        } catch (...) {
            std::exception_ptr failure = std::current_exception();
            if (!ring_.available()) {
                poisoned_ = true;
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& error) {
                    try {
                        poisoned_reason_ =
                            std::string("io_uring checkpoint backend is unusable: ") + error.what();
                    } catch (...) {}
                } catch (...) {}
            }
            if (active_.has_value()) {
                remove_known_directory_noexcept(active_->path_name);
                if (ring_.available()) {
                    try {
                        ring_.fsync(root_fd_.get(),
                                    "io_uring fsync failed checkpoint stage cleanup");
                    } catch (...) {}
                }
                active_.reset();
            }
            release_lock_noexcept();
            std::rethrow_exception(failure);
        }
    }

    void commit(const CheckpointStageKey& key) {
        std::lock_guard lock(mutex_);
        ensure_ready();
        require_active_key(key);

        if (!active_->renamed_to_generation) {
            rename_noreplace(root_fd_.get(), active_->path_name, active_->final_name);
            active_->path_name.swap(active_->final_name);
            active_->renamed_to_generation = true;
        }
        ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint generation publication");

        write_buffered_file(ring_, root_fd_.get(), active_->publish_path, active_->encoded);
        ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint manifest staging entry");
        try {
            write_buffered_file(ring_, root_fd_.get(), kPublicationUncertain, active_->encoded);
            ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint publication marker");
        } catch (...) {
            const std::exception_ptr marker_failure = std::current_exception();
            unlink_if_exists(root_fd_.get(), kPublicationUncertain);
            try {
                ring_.fsync(root_fd_.get(), "io_uring fsync failed publication marker cleanup");
            } catch (...) {}
            std::rethrow_exception(marker_failure);
        }

        const bool replacing = active_->old_generation.has_value();
        try {
            if (replacing) {
                rename_exchange(root_fd_.get(), active_->publish_path, "manifest");
            } else {
                rename_noreplace(root_fd_.get(), active_->publish_path, "manifest");
            }
        } catch (...) {
            unlink_if_exists(root_fd_.get(), kPublicationUncertain);
            ring_.fsync(root_fd_.get(), "io_uring fsync failed publication marker cleanup");
            throw;
        }
        try {
            ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint manifest publication");
        } catch (...) {
            const std::exception_ptr publication_failure = std::current_exception();
            try {
                if (replacing) {
                    rename_exchange(root_fd_.get(), active_->publish_path, "manifest");
                } else {
                    if (::renameat(root_fd_.get(), "manifest", root_fd_.get(),
                                   active_->publish_path.c_str()) != 0) {
                        throw_last_error("roll back initial checkpoint manifest publication");
                    }
                }
                ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint manifest rollback");
                unlink_if_exists(root_fd_.get(), kPublicationUncertain);
                ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint rollback marker cleanup");
            } catch (const std::exception& rollback_failure) {
                active_->publication_uncertain = true;
                poisoned_                      = true;
                try {
                    poisoned_reason_ =
                        std::string("checkpoint manifest publication failed and rollback "
                                    "was not durable: ") +
                        rollback_failure.what();
                } catch (...) {}
                throw CheckpointContractError(
                    poisoned_reason_.empty()
                        ? "checkpoint manifest publication failed and rollback was not durable"
                        : poisoned_reason_);
            }
            std::rethrow_exception(publication_failure);
        }

        committed_generation_.store(active_->manifest.generation, std::memory_order_release);
        try {
            unlink_if_exists(root_fd_.get(), kPublicationUncertain);
            ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint publication marker cleanup");
        } catch (...) {
            // The manifest itself is already durable and commit therefore succeeds. A marker that
            // survives a crash forces the next process to fail closed instead of guessing.
            poisoned_ = true;
            try {
                poisoned_reason_ =
                    "checkpoint committed but publication-marker cleanup did not complete";
            } catch (...) {}
        }
        // Publication is now durable. Cleanup cannot be reported as a failed commit; any residue is
        // removed under the same cross-instance lock by the next constructor/stage operation.
        ::unlinkat(root_fd_.get(), active_->publish_path.c_str(), 0);
        if (active_->old_generation.has_value()) {
            remove_known_directory_noexcept(*active_->old_generation);
        }
        try {
            ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint post-commit cleanup");
        } catch (...) {}
        active_.reset();
        release_lock_noexcept();
    }

    void abort(const CheckpointStageKey& key) noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (!active_.has_value() || !stage_keys_equal(active_->key, key)) { return; }
            if (!active_->publication_uncertain) {
                remove_known_directory_noexcept(active_->path_name);
                ::unlinkat(root_fd_.get(), active_->publish_path.c_str(), 0);
                try {
                    ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint abort cleanup");
                } catch (...) {}
            }
            active_.reset();
            release_lock_noexcept();
        } catch (...) {}
    }

    CheckpointImage load(const CheckpointExpectation& expected) {
        validate_manifest(expected.manifest, limits_);
        if (checkpoint_manifest_sha256(expected.manifest) != expected.manifest_sha256) {
            throw CheckpointContractError("trusted checkpoint expectation digest is invalid");
        }

        std::lock_guard lock(mutex_);
        ensure_ready();
        if (active_.has_value()) {
            throw CheckpointContractError(
                "cannot load while this backend has an active transaction");
        }
        acquire_lock(LOCK_SH);
        try {
            ensure_no_uncertain_publication();
            const std::optional<StoredManifest> current = read_current_manifest();
            if (!current.has_value()) {
                throw CheckpointContractError("no committed checkpoint manifest exists");
            }
            if (current->digest != expected.manifest_sha256) {
                throw CheckpointContractError(
                    "checkpoint manifest differs from the trusted expectation");
            }
            const CheckpointCompatibility compatibility =
                validate_checkpoint_compatibility(expected.manifest, current->manifest);
            if (!compatibility.compatible()) {
                throw CheckpointContractError("checkpoint is incompatible at " +
                                              std::string(compatibility.field));
            }
            committed_generation_.store(current->manifest.generation, std::memory_order_release);

            const std::string directory_name =
                generation_name(current->manifest.generation, current->digest);
            const int generation_fd = ::openat(root_fd_.get(), directory_name.c_str(),
                                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (generation_fd < 0) { throw_last_error("open committed checkpoint generation"); }
            UniqueFd directory(generation_fd);

            const std::optional<StoredManifest> generation_manifest =
                read_manifest_file(ring_, directory.get(), "manifest", limits_, false);
            if (!generation_manifest.has_value() ||
                generation_manifest->encoded.size != current->encoded.size ||
                !std::equal(generation_manifest->encoded.bytes.begin(),
                            generation_manifest->encoded.bytes.begin() + current->encoded.size,
                            current->encoded.bytes.begin())) {
                throw CheckpointContractError(
                    "checkpoint generation manifest differs from the published manifest");
            }

            std::vector<UniqueFd> payload_fds;
            payload_fds.reserve(current->manifest.payloads.size());
            for (const CheckpointPayloadDescriptor& descriptor : current->manifest.payloads) {
                const std::string name = payload_name(descriptor.kind);
                const int fd           = ::openat(directory.get(), name.c_str(),
                                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECT);
                if (fd < 0) { throw_last_error("open committed O_DIRECT checkpoint payload"); }
                payload_fds.emplace_back(fd);
                struct stat status{};
                if (::fstat(fd, &status) != 0) { throw_last_error("stat checkpoint payload"); }
                if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
                    static_cast<std::uint64_t>(status.st_size) != descriptor.bytes) {
                    throw CheckpointContractError(
                        "checkpoint payload length differs from the trusted manifest");
                }
            }

            CheckpointImage image;
            image.manifest = current->manifest;
            image.payloads.reserve(current->manifest.payloads.size());
            for (const CheckpointPayloadDescriptor& descriptor : current->manifest.payloads) {
                CheckpointPayload payload;
                payload.kind = descriptor.kind;
                payload.bytes.resize(static_cast<std::size_t>(descriptor.bytes));
                image.payloads.push_back(std::move(payload));
            }

            std::vector<DirectReadFile> reads;
            reads.reserve(image.payloads.size());
            for (std::size_t index = 0; index < image.payloads.size(); ++index) {
                reads.push_back(DirectReadFile{payload_fds[index].get(), &image.payloads[index],
                                               &image.manifest.payloads[index]});
            }
            direct_read_payloads(ring_, reads, alignment_);
            release_lock_noexcept();
            return image;
        } catch (...) {
            release_lock_noexcept();
            throw;
        }
    }

private:
    struct ActiveStage {
        CheckpointStageKey key;
        CheckpointManifestV1 manifest;
        EncodedManifest encoded;
        std::string path_name;
        std::string final_name;
        std::string publish_path;
        std::optional<std::string> old_generation;
        bool renamed_to_generation = false;
        bool publication_uncertain = false;
    };

    void ensure_ready() const {
        if (poisoned_) {
            throw CheckpointContractError(poisoned_reason_.empty()
                                              ? "checkpoint backend publication state is uncertain"
                                              : poisoned_reason_);
        }
    }

    void require_active_key(const CheckpointStageKey& key) const {
        if (!active_.has_value() || !stage_keys_equal(active_->key, key)) {
            throw CheckpointContractError(
                "checkpoint commit key does not identify the active transaction");
        }
    }

    void ensure_no_uncertain_publication() const {
        if (entry_exists(root_fd_.get(), kPublicationUncertain)) {
            throw CheckpointContractError(
                "checkpoint manifest publication is persistently marked uncertain");
        }
    }

    void acquire_lock(int operation) {
        if (lock_held_) {
            throw CheckpointContractError("checkpoint single-flight lock is already held");
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(limits_.lock_timeout_ms);
        for (;;) {
            if (::flock(lock_fd_.get(), operation | LOCK_NB) == 0) {
                lock_held_ = true;
                return;
            }
            if (errno == EINTR) { continue; }
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                throw_last_error("acquire checkpoint single-flight lock");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw CheckpointContractError("timed out acquiring checkpoint single-flight lock");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void release_lock_noexcept() noexcept {
        if (!lock_held_) { return; }
        ::flock(lock_fd_.get(), LOCK_UN);
        lock_held_ = false;
    }

    std::optional<StoredManifest> read_current_manifest() {
        return read_manifest_file(ring_, root_fd_.get(), "manifest", limits_, true);
    }

    bool remove_known_directory(std::string_view name, bool tolerate_foreign = false) {
        const std::string path(name);
        const int fd =
            ::openat(root_fd_.get(), path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            if (errno == ENOENT) { return true; }
            if (tolerate_foreign && (errno == ENOTDIR || errno == ELOOP)) { return false; }
            throw_last_error("open checkpoint generation for cleanup");
        }
        UniqueFd directory(fd);
        for (std::size_t kind = 0; kind < kPayloadKindCount; ++kind) {
            unlink_if_exists(directory.get(), "payload-" + std::to_string(kind));
        }
        unlink_if_exists(directory.get(), "manifest");
        directory.reset();
        if (::unlinkat(root_fd_.get(), path.c_str(), AT_REMOVEDIR) != 0) {
            if (errno == ENOENT) { return true; }
            if (tolerate_foreign && errno == ENOTEMPTY) { return false; }
            throw_last_error("remove checkpoint generation directory");
        }
        return true;
    }

    void remove_known_directory_noexcept(std::string_view name) noexcept {
        try {
            (void)remove_known_directory(name);
        } catch (...) {}
    }

    void cleanup_orphans(const std::optional<StoredManifest>& current) {
        const std::optional<std::string> current_generation =
            current.has_value() ? std::optional<std::string>(generation_name(
                                      current->manifest.generation, current->digest))
                                : std::nullopt;

        const int duplicate = ::dup(root_fd_.get());
        if (duplicate < 0) { throw_last_error("duplicate checkpoint root for cleanup"); }
        DIR* raw_directory = ::fdopendir(duplicate);
        if (raw_directory == nullptr) {
            const int error = errno;
            ::close(duplicate);
            throw_system_error("enumerate checkpoint root for cleanup", error);
        }
        std::unique_ptr<DIR, int (*)(DIR*)> directory(raw_directory, ::closedir);
        std::vector<std::string> stale_directories;
        std::vector<std::string> stale_files;
        errno = 0;
        while (dirent* entry = ::readdir(directory.get())) {
            const std::string_view name(entry->d_name);
            if (name == "." || name == "..") { continue; }
            if (owned_stage_name(name)) {
                stale_directories.emplace_back(name);
            } else if (owned_generation_name(name) &&
                       (!current_generation.has_value() || name != *current_generation)) {
                stale_directories.emplace_back(name);
            } else if (owned_publish_name(name)) {
                stale_files.emplace_back(name);
            }
        }
        if (errno != 0) { throw_last_error("enumerate checkpoint root for cleanup"); }
        directory.reset();

        bool changed = false;
        for (const std::string& name : stale_directories) {
            changed |= remove_known_directory(name, true);
        }
        for (const std::string& name : stale_files) { unlink_if_exists(root_fd_.get(), name); }
        changed |= !stale_files.empty();
        if (changed) { ring_.fsync(root_fd_.get(), "io_uring fsync checkpoint orphan cleanup"); }
    }

    std::filesystem::path root_path_;
    IoUringCheckpointLimits limits_;
    LinuxCheckpointEnvironment environment_;
    UniqueFd root_fd_;
    UniqueFd lock_fd_;
    NativeIoUring ring_;
    DirectIoAlignment alignment_;
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> committed_generation_{0};
    std::optional<ActiveStage> active_;
    std::string poisoned_reason_;
    bool poisoned_  = false;
    bool lock_held_ = false;
};

namespace {

class CompletedCheckpointRead final : public ContinuationCheckpointReadCompletion {
public:
    void wait() override {}
};

class IoUringContinuationReadQueue final : public ContinuationCheckpointReadQueue {
public:
    explicit IoUringContinuationReadQueue(const std::filesystem::path& root)
        : root_(std::filesystem::weakly_canonical(root)) {
        const LinuxCheckpointEnvironment environment = detect_environment();
        UniqueFd root_fd                             = open_root(root_);
        require_local_filesystem(root_fd.get(), environment);
        ring_.initialize();
        alignment_ = verify_storage_capabilities(root_fd.get(), ring_);
    }

    [[nodiscard]] bool available() const noexcept override { return true; }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "io_uring-odirect";
    }

    [[nodiscard]] std::string_view unavailable_reason() const noexcept override { return {}; }

    [[nodiscard]] std::unique_ptr<ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const ContinuationCheckpointReadRequest> requests) override {
        if (requests.empty()) {
            throw CheckpointContractError("io_uring checkpoint read batch is empty");
        }
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path);
        if (!contains(canonical)) {
            throw CheckpointContractError("io_uring checkpoint read escaped its configured root");
        }

        UniqueFd file(::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECT));
        if (!file) { throw_last_error("open io_uring checkpoint payload"); }
        struct stat status{};
        if (::fstat(file.get(), &status) != 0) {
            throw_last_error("stat io_uring checkpoint payload");
        }
        if (!S_ISREG(status.st_mode) || status.st_size < 0) {
            throw CheckpointContractError("io_uring checkpoint payload is not a regular file");
        }
        const std::uint64_t file_bytes = static_cast<std::uint64_t>(status.st_size);
        const std::size_t allocation_alignment =
            std::max(alignment_.memory, static_cast<std::size_t>(alignof(std::max_align_t)));
        const std::size_t buffer_bytes = static_cast<std::size_t>(
            round_up(kDirectChunkBytes + alignment_.offset, alignment_.offset));
        AlignedBuffer buffer(allocation_alignment, buffer_bytes);

        std::lock_guard lock(mutex_);
        for (const ContinuationCheckpointReadRequest& request : requests) {
            if (request.destination.empty() || request.file_offset > file_bytes ||
                request.destination.size() > file_bytes - request.file_offset) {
                throw CheckpointContractError("io_uring checkpoint read exceeds the payload file");
            }
            std::uint64_t offset = request.file_offset;
            std::size_t copied   = 0;
            while (copied < request.destination.size()) {
                const std::uint64_t aligned_offset =
                    offset & ~(static_cast<std::uint64_t>(alignment_.offset) - 1U);
                const std::size_t prefix = static_cast<std::size_t>(offset - aligned_offset);
                const std::size_t logical =
                    std::min(request.destination.size() - copied,
                             std::min(kDirectChunkBytes, buffer.size() - prefix));
                const std::size_t required = prefix + logical;
                const std::size_t submitted =
                    static_cast<std::size_t>(round_up(required, alignment_.offset));
                if (submitted > buffer.size() ||
                    submitted > std::numeric_limits<std::uint32_t>::max()) {
                    throw CheckpointContractError("io_uring checkpoint read chunk is invalid");
                }
                const std::int32_t result = ring_.execute_one(
                    RingRequest{IORING_OP_READ, file.get(), aligned_offset, buffer.data(),
                                static_cast<std::uint32_t>(submitted), 0});
                if (result < 0) { throw_system_error("io_uring checkpoint payload read", -result); }
                if (static_cast<std::size_t>(result) < required) {
                    throw CheckpointContractError("io_uring checkpoint payload read was short");
                }
                std::memcpy(request.destination.data() + copied, buffer.data() + prefix, logical);
                copied += logical;
                offset += logical;
            }
        }
        return std::make_unique<CompletedCheckpointRead>();
    }

private:
    [[nodiscard]] bool contains(const std::filesystem::path& path) const {
        auto root      = root_.begin();
        auto candidate = path.begin();
        for (; root != root_.end(); ++root, ++candidate) {
            if (candidate == path.end() || *candidate != *root) { return false; }
        }
        return candidate != path.end();
    }

    std::filesystem::path root_;
    NativeIoUring ring_;
    DirectIoAlignment alignment_;
    std::mutex mutex_;
};

} // namespace

IoUringCheckpointCapability
probe_io_uring_checkpoint_capability(const std::filesystem::path& root) noexcept {
    IoUringCheckpointCapability capability;
    capability.environment = detect_environment();
    try {
        UniqueFd root_fd = open_root(root);
        require_local_filesystem(root_fd.get(), capability.environment);
        NativeIoUring ring;
        ring.initialize();
        const DirectIoAlignment alignment = verify_storage_capabilities(root_fd.get(), ring);
        capability.available              = true;
        capability.memory_alignment       = alignment.memory;
        capability.offset_alignment       = alignment.offset;
    } catch (const std::exception& error) { capability.reason = error.what(); } catch (...) {
        capability.reason = "unknown Linux checkpoint capability failure";
    }
    return capability;
}

std::shared_ptr<ContinuationCheckpointReadQueue>
make_io_uring_checkpoint_read_queue(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw CheckpointContractError("create io_uring checkpoint root: " + error.message());
    }
    return std::make_shared<IoUringContinuationReadQueue>(root);
}

IoUringCheckpointBackend::IoUringCheckpointBackend(std::filesystem::path root,
                                                   IoUringCheckpointLimits limits)
    : impl_(std::make_unique<Impl>(std::move(root), limits)) {}

IoUringCheckpointBackend::~IoUringCheckpointBackend() = default;

CheckpointBackendCapabilities IoUringCheckpointBackend::capabilities() const noexcept {
    // O_DIRECT file I/O is not a storage-to-GPU path; the frozen backend API owns host bytes.
    return {true, true, false};
}

std::uint64_t IoUringCheckpointBackend::committed_generation() const noexcept {
    return impl_->committed_generation();
}

void IoUringCheckpointBackend::stage(const CheckpointManifestV1& manifest,
                                     std::span<const CheckpointPayload> payloads,
                                     const CheckpointStageKey& key) {
    impl_->stage(manifest, payloads, key);
}

void IoUringCheckpointBackend::commit(const CheckpointStageKey& key) { impl_->commit(key); }

void IoUringCheckpointBackend::abort(const CheckpointStageKey& key) noexcept { impl_->abort(key); }

CheckpointImage IoUringCheckpointBackend::load(const CheckpointExpectation& expected) {
    return impl_->load(expected);
}

} // namespace ninfer::runtime
