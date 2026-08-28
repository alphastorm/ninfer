#include "runtime/windows/direct_storage_checkpoint_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <cwctype>
#endif

namespace ninfer::runtime::windows {
namespace {

constexpr std::uint64_t kPayloadAlignment        = 4096;
constexpr std::uint64_t kDirectStorageChunkBytes = 32ULL << 20;
constexpr std::size_t kPayloadKindCount          = 4;
constexpr std::size_t kManifestPrefixBytes       = 164;
constexpr std::size_t kManifestDescriptorBytes   = 41;
constexpr std::size_t kMaxManifestBytes =
    kManifestPrefixBytes + kPayloadKindCount * kManifestDescriptorBytes;
constexpr std::size_t kMaxDirectStorageRequests = 0x2000 - 2;

[[noreturn]] void fail(std::string message) {
    throw CheckpointContractError(std::move(message));
}

bool keys_equal(const CheckpointStageKey& left, const CheckpointStageKey& right) noexcept {
    return left.journal_version == right.journal_version && left.generation == right.generation &&
           left.manifest_sha256 == right.manifest_sha256;
}

bool manifest_well_formed(const CheckpointManifestV1& manifest) noexcept {
    if (manifest.magic != kCheckpointMagic || manifest.schema_version != kCheckpointSchemaVersion ||
        manifest.journal_version != kCheckpointJournalVersion || manifest.generation == 0 ||
        manifest.identity.token_count == 0 || manifest.identity.context_capacity == 0 ||
        manifest.identity.token_count > manifest.identity.context_capacity ||
        manifest.payloads.empty() || manifest.payloads.size() > kPayloadKindCount) {
        return false;
    }

    std::array<bool, kPayloadKindCount> observed{};
    for (const CheckpointPayloadDescriptor& descriptor : manifest.payloads) {
        const std::size_t kind = static_cast<std::size_t>(descriptor.kind);
        if (kind >= observed.size() || observed[kind] || descriptor.bytes == 0) { return false; }
        observed[kind] = true;
    }
    return true;
}

template <typename Integer>
void append_integer(std::array<std::byte, kMaxManifestBytes>& output, std::size_t& cursor,
                    Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    std::uint64_t remaining = value;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output[cursor + sizeof(Integer) - 1 - index] =
            static_cast<std::byte>(remaining & 0xffU);
        remaining >>= 8U;
    }
    cursor += sizeof(Integer);
}

void append_digest(std::array<std::byte, kMaxManifestBytes>& output, std::size_t& cursor,
                   const CheckpointDigest& digest) {
    for (const std::uint8_t value : digest) { output[cursor++] = static_cast<std::byte>(value); }
}

struct CanonicalManifestBytes {
    std::array<std::byte, kMaxManifestBytes> storage{};
    std::size_t size = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span(storage).first(size);
    }
};

CanonicalManifestBytes serialize_manifest(const CheckpointManifestV1& manifest) {
    if (!manifest_well_formed(manifest)) { fail("checkpoint manifest structure is invalid"); }

    CanonicalManifestBytes encoded;
    append_integer(encoded.storage, encoded.size, manifest.magic);
    append_integer(encoded.storage, encoded.size, manifest.schema_version);
    append_integer(encoded.storage, encoded.size, manifest.journal_version);
    append_integer(encoded.storage, encoded.size, manifest.generation);
    append_digest(encoded.storage, encoded.size, manifest.identity.model);
    append_digest(encoded.storage, encoded.size, manifest.identity.runtime_source);
    append_digest(encoded.storage, encoded.size, manifest.identity.deployment_profile);
    append_digest(encoded.storage, encoded.size, manifest.identity.layout);
    append_integer(encoded.storage, encoded.size, manifest.identity.token_count);
    append_integer(encoded.storage, encoded.size, manifest.identity.context_capacity);
    append_integer(encoded.storage, encoded.size,
                   static_cast<std::uint32_t>(manifest.payloads.size()));
    for (const CheckpointPayloadDescriptor& descriptor : manifest.payloads) {
        append_integer(encoded.storage, encoded.size, static_cast<std::uint8_t>(descriptor.kind));
        append_integer(encoded.storage, encoded.size, descriptor.bytes);
        append_digest(encoded.storage, encoded.size, descriptor.sha256);
    }
    return encoded;
}

class ManifestCursor {
public:
    explicit ManifestCursor(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename Integer>
    [[nodiscard]] Integer integer() {
        static_assert(std::is_unsigned_v<Integer>);
        if (bytes_.size() - cursor_ < sizeof(Integer)) {
            fail("committed checkpoint manifest is truncated");
        }
        Integer value = 0;
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            value = static_cast<Integer>((value << 8U) |
                                         std::to_integer<std::uint8_t>(bytes_[cursor_++]));
        }
        return value;
    }

    [[nodiscard]] CheckpointDigest digest() {
        if (bytes_.size() - cursor_ < CheckpointDigest{}.size()) {
            fail("committed checkpoint manifest is truncated");
        }
        CheckpointDigest value{};
        for (std::uint8_t& byte : value) {
            byte = std::to_integer<std::uint8_t>(bytes_[cursor_++]);
        }
        return value;
    }

    [[nodiscard]] bool finished() const noexcept { return cursor_ == bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
    std::size_t cursor_ = 0;
};

CheckpointManifestV1 parse_manifest(std::span<const std::byte> bytes) {
    if (bytes.size() < kManifestPrefixBytes + kManifestDescriptorBytes ||
        bytes.size() > kMaxManifestBytes) {
        fail("committed checkpoint manifest has an invalid bounded size");
    }

    ManifestCursor cursor(bytes);
    CheckpointManifestV1 manifest;
    manifest.magic                       = cursor.integer<std::uint64_t>();
    manifest.schema_version              = cursor.integer<std::uint32_t>();
    manifest.journal_version             = cursor.integer<std::uint32_t>();
    manifest.generation                  = cursor.integer<std::uint64_t>();
    manifest.identity.model              = cursor.digest();
    manifest.identity.runtime_source     = cursor.digest();
    manifest.identity.deployment_profile = cursor.digest();
    manifest.identity.layout             = cursor.digest();
    manifest.identity.token_count        = cursor.integer<std::uint32_t>();
    manifest.identity.context_capacity   = cursor.integer<std::uint32_t>();
    const std::uint32_t count            = cursor.integer<std::uint32_t>();
    if (count == 0 || count > kPayloadKindCount ||
        bytes.size() != kManifestPrefixBytes + count * kManifestDescriptorBytes) {
        fail("committed checkpoint manifest has an invalid descriptor count");
    }

    manifest.payloads.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        CheckpointPayloadDescriptor descriptor;
        const std::uint8_t kind = cursor.integer<std::uint8_t>();
        if (kind >= kPayloadKindCount) { fail("committed checkpoint manifest has an invalid payload kind"); }
        descriptor.kind   = static_cast<CheckpointPayloadKind>(kind);
        descriptor.bytes  = cursor.integer<std::uint64_t>();
        descriptor.sha256 = cursor.digest();
        manifest.payloads.push_back(descriptor);
    }
    if (!cursor.finished() || !manifest_well_formed(manifest)) {
        fail("committed checkpoint manifest structure is invalid");
    }
    return manifest;
}

std::string generation_hex(std::uint64_t generation) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[result.size() - 1 - index] = digits[generation & 0xfU];
        generation >>= 4U;
    }
    return result;
}

std::string transaction_name(const CheckpointStageKey& key) {
    return generation_hex(key.generation) + "-" + sha256_hex(key.manifest_sha256);
}

std::uint64_t align_payload_offset(std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - (kPayloadAlignment - 1)) {
        fail("checkpoint payload layout overflows its canonical offset domain");
    }
    return (value + (kPayloadAlignment - 1)) & ~(kPayloadAlignment - 1);
}

struct PayloadLayout {
    std::array<std::uint64_t, kPayloadKindCount> offsets{};
    std::size_t count      = 0;
    std::uint64_t bytes    = 0;
    std::size_t requests   = 0;
};

PayloadLayout payload_layout(const std::vector<CheckpointPayloadDescriptor>& descriptors,
                             std::uint64_t max_checkpoint_bytes) {
    PayloadLayout layout;
    layout.count = descriptors.size();
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        const CheckpointPayloadDescriptor& descriptor = descriptors[index];
        const std::uint64_t offset = index == 0 ? 0 : align_payload_offset(layout.bytes);
        if (descriptor.bytes > std::numeric_limits<std::uint64_t>::max() - offset) {
            fail("checkpoint payload layout overflows its canonical size domain");
        }
        layout.offsets[index] = offset;
        layout.bytes          = offset + descriptor.bytes;
        if (layout.bytes > max_checkpoint_bytes) {
            fail("checkpoint payload bytes exceed the configured load bound");
        }

        const std::uint64_t request_count =
            descriptor.bytes / kDirectStorageChunkBytes +
            (descriptor.bytes % kDirectStorageChunkBytes != 0 ? 1 : 0);
        if (request_count > kMaxDirectStorageRequests - layout.requests) {
            fail("checkpoint payload requires more DirectStorage requests than one transaction allows");
        }
        layout.requests += static_cast<std::size_t>(request_count);
    }
    return layout;
}

void require_payload_shapes(const CheckpointManifestV1& manifest,
                            std::span<const CheckpointPayload> payloads) {
    if (manifest.payloads.size() != payloads.size()) {
        fail("checkpoint payload count differs from the trusted manifest");
    }
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        if (manifest.payloads[index].kind != payloads[index].kind ||
            manifest.payloads[index].bytes != payloads[index].bytes.size()) {
            fail("checkpoint payload shape differs from the trusted manifest");
        }
    }
}

using RootKey = std::filesystem::path::string_type;

struct SharedRootState {
    std::mutex mutex;
    std::atomic<std::uint64_t> committed_generation{0};
    const void* active_owner = nullptr;
    std::optional<CheckpointStageKey> active_key;
};

std::mutex root_registry_mutex;
std::unordered_map<RootKey, std::weak_ptr<SharedRootState>> root_registry;

RootKey root_key(const std::filesystem::path& root) {
    RootKey key = root.native();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
#endif
    return key;
}

std::shared_ptr<SharedRootState> shared_root_state(const std::filesystem::path& root) {
    std::lock_guard lock(root_registry_mutex);
    const RootKey key = root_key(root);
    if (const auto found = root_registry.find(key); found != root_registry.end()) {
        if (std::shared_ptr<SharedRootState> state = found->second.lock()) { return state; }
        root_registry.erase(found);
    }
    auto state         = std::make_shared<SharedRootState>();
    root_registry[key] = state;
    return state;
}

} // namespace

class DirectStorageCheckpointBackend::Impl {
public:
    Impl(DirectStorageCheckpointConfig config,
         std::shared_ptr<CheckpointFileSystem> file_system,
         std::shared_ptr<CheckpointReadQueue> read_queue)
        : config_(std::move(config)), file_system_(std::move(file_system)),
          read_queue_(std::move(read_queue)) {
        if (config_.directory.empty()) { fail("checkpoint directory must be non-empty"); }
        if (config_.max_checkpoint_bytes == 0) {
            fail("checkpoint load bound must be nonzero");
        }
        if (!file_system_) { fail("checkpoint filesystem is unavailable"); }
        if (!read_queue_) { fail("Windows DirectStorage 1.3 queue is unavailable"); }
        if (!read_queue_->available()) {
            fail("Windows DirectStorage 1.3 is unavailable: " +
                 std::string(read_queue_->unavailable_reason()));
        }

        root_       = std::filesystem::absolute(config_.directory).lexically_normal();
        staging_    = root_ / ".ninfer-checkpoint-staging-v1";
        manifest_   = root_ / "checkpoint.manifest.v1";
        shared_root_ = shared_root_state(root_);

        file_system_->ensure_directory(root_);
        file_system_->ensure_directory(staging_);
        std::lock_guard root_lock(shared_root_->mutex);
        if (shared_root_->active_owner != nullptr) {
            fail("checkpoint backend construction overlaps a staged transaction");
        }
        std::unique_ptr<CheckpointDirectoryLock> directory_lock =
            file_system_->lock_directory(root_);
        const std::optional<CommittedCheckpoint> committed = read_committed();
        shared_root_->committed_generation.store(
            committed ? committed->manifest.generation : 0, std::memory_order_release);
    }

    ~Impl() { cleanup_staged_noexcept(); }

    [[nodiscard]] std::uint64_t committed_generation() const noexcept {
        return shared_root_->committed_generation.load(std::memory_order_acquire);
    }

    void stage(const CheckpointManifestV1& manifest,
               std::span<const CheckpointPayload> payloads,
               const CheckpointStageKey& key) {
        const CanonicalManifestBytes encoded = serialize_manifest(manifest);
        const CheckpointDigest digest        = sha256(encoded.bytes());
        if (key.journal_version != kCheckpointJournalVersion ||
            key.journal_version != manifest.journal_version || key.generation == 0 ||
            key.generation != manifest.generation || key.manifest_sha256 != digest ||
            digest != checkpoint_manifest_sha256(manifest)) {
            fail("checkpoint stage key does not identify the canonical manifest");
        }
        require_payload_shapes(manifest, payloads);
        const PayloadLayout layout = payload_layout(manifest.payloads, config_.max_checkpoint_bytes);

        std::lock_guard root_lock(shared_root_->mutex);
        if (shared_root_->active_owner != nullptr) {
            fail("another checkpoint backend instance owns the staged transaction");
        }
        std::unique_ptr<CheckpointDirectoryLock> directory_lock =
            file_system_->lock_directory(root_);
        if (staged_) { fail("checkpoint backend already owns a staged transaction"); }
        const std::optional<CommittedCheckpoint> committed = read_committed();
        const std::uint64_t generation = committed ? committed->manifest.generation : 0;
        shared_root_->committed_generation.store(generation, std::memory_order_release);
        if (key.generation <= generation) {
            fail("checkpoint stage generation does not advance the committed manifest");
        }

        StagedCheckpoint candidate;
        candidate.key             = key;
        const std::string name    = transaction_name(key);
        candidate.staged_payload  = staging_ / (name + ".payload.stage");
        candidate.staged_manifest = staging_ / (name + ".manifest.stage");
        candidate.final_payload   = root_ / ("checkpoint-" + name + ".payload");
        candidate.directory_lock  = std::move(directory_lock);

        remove_checked(candidate.staged_payload);
        remove_checked(candidate.staged_manifest);
        try {
            file_system_->write_payloads_durable(
                candidate.staged_payload, payloads,
                std::span(layout.offsets).first(layout.count), layout.bytes);
            file_system_->write_bytes_durable(candidate.staged_manifest, encoded.bytes());
        } catch (...) {
            const std::exception_ptr original = std::current_exception();
            try {
                remove_checked(candidate.staged_manifest);
                remove_checked(candidate.staged_payload);
            } catch (...) {
                fail("checkpoint stage failed and its private staging files could not be removed");
            }
            std::rethrow_exception(original);
        }
        staged_ = std::move(candidate);
        shared_root_->active_owner = this;
        shared_root_->active_key   = key;
    }

    void commit(const CheckpointStageKey& key) {
        std::lock_guard root_lock(shared_root_->mutex);
        if (!staged_ || !keys_equal(staged_->key, key) ||
            shared_root_->active_owner != this || !shared_root_->active_key ||
            !keys_equal(*shared_root_->active_key, key)) {
            fail("checkpoint commit key does not identify the owned staged transaction");
        }

        const std::optional<CommittedCheckpoint> prior = read_committed();
        const std::uint64_t generation = prior ? prior->manifest.generation : 0;
        shared_root_->committed_generation.store(generation, std::memory_order_release);
        if (key.generation <= generation) {
            fail("checkpoint commit generation does not advance the committed manifest");
        }

        const std::filesystem::path prior_payload =
            prior ? prior->payload_path : std::filesystem::path{};
        const std::filesystem::path committed_payload = staged_->final_payload;
        file_system_->atomic_replace_durable(staged_->staged_payload, staged_->final_payload);
        staged_->payload_published = true;

        // No throwing work may follow this publication. A successful replacement makes the new
        // manifest authoritative and its generation visible to every in-process backend instance.
        file_system_->atomic_replace_durable(staged_->staged_manifest, manifest_);
        shared_root_->active_owner = nullptr;
        shared_root_->active_key.reset();
        staged_.reset();
        shared_root_->committed_generation.store(key.generation, std::memory_order_release);
        if (!prior_payload.empty() && prior_payload != committed_payload) {
            file_system_->remove_file(prior_payload);
        }
    }

    void abort(const CheckpointStageKey& key) noexcept {
        try {
            std::lock_guard root_lock(shared_root_->mutex);
            if (!staged_ || !keys_equal(staged_->key, key) ||
                shared_root_->active_owner != this || !shared_root_->active_key ||
                !keys_equal(*shared_root_->active_key, key)) {
                return;
            }
            file_system_->remove_file(staged_->staged_manifest);
            file_system_->remove_file(staged_->staged_payload);
            if (staged_->payload_published) { file_system_->remove_file(staged_->final_payload); }
            shared_root_->active_owner = nullptr;
            shared_root_->active_key.reset();
            staged_.reset();
        } catch (...) {
        }
    }

    CheckpointImage load(const CheckpointExpectation& expected) {
        const CanonicalManifestBytes trusted = serialize_manifest(expected.manifest);
        if (sha256(trusted.bytes()) != expected.manifest_sha256 ||
            checkpoint_manifest_sha256(expected.manifest) != expected.manifest_sha256) {
            fail("trusted checkpoint expectation digest is invalid");
        }
        const PayloadLayout expected_layout =
            payload_layout(expected.manifest.payloads, config_.max_checkpoint_bytes);

        std::lock_guard root_lock(shared_root_->mutex);
        if (shared_root_->active_owner != nullptr) {
            fail("checkpoint load cannot overlap an owned staged transaction");
        }
        std::unique_ptr<CheckpointDirectoryLock> directory_lock =
            file_system_->lock_directory(root_);
        const std::optional<CommittedCheckpoint> committed = read_committed();
        if (!committed) { fail("no committed checkpoint manifest is available"); }
        shared_root_->committed_generation.store(committed->manifest.generation,
                                                  std::memory_order_release);

        const CheckpointCompatibility compatibility =
            validate_checkpoint_compatibility(expected.manifest, committed->manifest);
        if (!compatibility.compatible()) {
            fail("committed checkpoint is incompatible at " + std::string(compatibility.field));
        }
        if (committed->key.manifest_sha256 != expected.manifest_sha256) {
            fail("committed checkpoint manifest differs from the trusted expectation");
        }

        const std::uint64_t observed_size = file_system_->file_size(committed->payload_path);
        if (observed_size != expected_layout.bytes) {
            fail("committed checkpoint payload file has an invalid bounded size");
        }

        CheckpointImage image;
        image.manifest = committed->manifest;
        image.payloads.reserve(image.manifest.payloads.size());
        for (const CheckpointPayloadDescriptor& descriptor : image.manifest.payloads) {
            if (descriptor.bytes > std::numeric_limits<std::size_t>::max()) {
                fail("checkpoint payload does not fit the host allocation domain");
            }
            CheckpointPayload payload;
            payload.kind = descriptor.kind;
            payload.bytes.resize(static_cast<std::size_t>(descriptor.bytes));
            image.payloads.push_back(std::move(payload));
        }

        std::vector<CheckpointReadRequest> requests;
        requests.reserve(expected_layout.requests);
        for (std::size_t payload_index = 0; payload_index < image.payloads.size(); ++payload_index) {
            CheckpointPayload& payload = image.payloads[payload_index];
            std::uint64_t remaining    = payload.bytes.size();
            std::uint64_t file_offset  = expected_layout.offsets[payload_index];
            std::size_t destination_offset = 0;
            while (remaining != 0) {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, kDirectStorageChunkBytes));
                requests.push_back({
                    file_offset,
                    std::span(payload.bytes).subspan(destination_offset, chunk),
                });
                remaining -= chunk;
                file_offset += chunk;
                destination_offset += chunk;
            }
        }

        std::unique_ptr<CheckpointReadCompletion> completion =
            read_queue_->submit(committed->payload_path, requests);
        if (!completion) { fail("DirectStorage queue returned no completion object"); }
        completion->wait();
        return image;
    }

private:
    struct CommittedCheckpoint {
        CheckpointManifestV1 manifest;
        CheckpointStageKey key;
        std::filesystem::path payload_path;
    };

    struct StagedCheckpoint {
        CheckpointStageKey key;
        std::filesystem::path staged_payload;
        std::filesystem::path staged_manifest;
        std::filesystem::path final_payload;
        std::unique_ptr<CheckpointDirectoryLock> directory_lock;
        bool payload_published = false;
    };

    [[nodiscard]] std::optional<CommittedCheckpoint> read_committed() {
        if (!file_system_->file_exists(manifest_)) { return std::nullopt; }
        const std::uint64_t size = file_system_->file_size(manifest_);
        if (size < kManifestPrefixBytes + kManifestDescriptorBytes ||
            size > kMaxManifestBytes) {
            fail("committed checkpoint manifest exceeds its bounded canonical size");
        }

        std::array<std::byte, kMaxManifestBytes> bytes{};
        const std::span<std::byte> manifest_bytes =
            std::span(bytes).first(static_cast<std::size_t>(size));
        file_system_->read_exact(manifest_, manifest_bytes);
        CheckpointManifestV1 parsed = parse_manifest(manifest_bytes);
        const CheckpointDigest digest = sha256(std::as_bytes(std::span(manifest_bytes)));
        if (digest != checkpoint_manifest_sha256(parsed)) {
            fail("committed checkpoint manifest is not canonical");
        }

        CheckpointStageKey key{parsed.journal_version, parsed.generation, digest};
        const std::string name = transaction_name(key);
        return CommittedCheckpoint{
            std::move(parsed), key, root_ / ("checkpoint-" + name + ".payload")};
    }

    void remove_checked(const std::filesystem::path& path) {
        if (!file_system_->file_exists(path)) { return; }
        file_system_->remove_file(path);
        if (file_system_->file_exists(path)) {
            fail("checkpoint private staging file could not be removed");
        }
    }

    void cleanup_staged_noexcept() noexcept {
        if (!staged_) { return; }
        abort(staged_->key);
    }

    DirectStorageCheckpointConfig config_;
    std::shared_ptr<CheckpointFileSystem> file_system_;
    std::shared_ptr<CheckpointReadQueue> read_queue_;
    std::filesystem::path root_;
    std::filesystem::path staging_;
    std::filesystem::path manifest_;
    std::shared_ptr<SharedRootState> shared_root_;
    std::optional<StagedCheckpoint> staged_;
};

DirectStorageCheckpointBackend::DirectStorageCheckpointBackend(
    DirectStorageCheckpointConfig config, std::shared_ptr<CheckpointFileSystem> file_system,
    std::shared_ptr<CheckpointReadQueue> read_queue)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(file_system),
                                   std::move(read_queue))) {}

DirectStorageCheckpointBackend::~DirectStorageCheckpointBackend() = default;

CheckpointBackendCapabilities DirectStorageCheckpointBackend::capabilities() const noexcept {
    // The frozen CheckpointBackend load contract returns host-owned vectors. The implementation
    // uses DirectStorage for those reads but does not misreport them as direct-to-device restores.
    return {true, true, false};
}

std::uint64_t DirectStorageCheckpointBackend::committed_generation() const noexcept {
    return impl_->committed_generation();
}

void DirectStorageCheckpointBackend::stage(const CheckpointManifestV1& manifest,
                                           std::span<const CheckpointPayload> payloads,
                                           const CheckpointStageKey& key) {
    impl_->stage(manifest, payloads, key);
}

void DirectStorageCheckpointBackend::commit(const CheckpointStageKey& key) {
    impl_->commit(key);
}

void DirectStorageCheckpointBackend::abort(const CheckpointStageKey& key) noexcept {
    impl_->abort(key);
}

CheckpointImage DirectStorageCheckpointBackend::load(const CheckpointExpectation& expected) {
    return impl_->load(expected);
}

} // namespace ninfer::runtime::windows
