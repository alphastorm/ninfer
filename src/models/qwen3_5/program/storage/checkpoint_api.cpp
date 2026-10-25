#include "models/qwen3_5/program/program_impl.h"
#include "models/qwen3_5/program/storage/checkpoint_codec.h"
#include "models/common/qwen/vision/control.h"
#include "runtime/contract/continuation_checkpoint.h"
#include "core/sha256.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace ninfer::models::qwen3_5 {
namespace {
using namespace runtime;
using detail::ProgramImpl;
using Json = nlohmann::json;
namespace codec = detail::checkpoint_codec;

struct Refusal {
    DurablePrepareState state;
    DurableReason reason;
};
void check(bool value, DurableReason reason = DurableReason::InvalidIndex) {
    if (!value) throw Refusal{DurablePrepareState::Corrupt, reason};
}
void check_read(bool value, const ContinuationCheckpointReader& reader) {
    if (value) return;
    const auto reason = reader.failure_reason();
    if (reason == DurableReason::IoUnavailable)
        throw Refusal{DurablePrepareState::Unavailable, reason};
    if (reason == DurableReason::None)
        throw std::logic_error("checkpoint reader failed without a reason");
    throw Refusal{DurablePrepareState::Corrupt, reason};
}
void capacity(bool value) {
    if (!value) throw Refusal{DurablePrepareState::CapacityRefused, DurableReason::Capacity};
}
std::string digest(std::string_view value) {
    return crypto::sha256_hex(crypto::sha256(std::as_bytes(std::span(value.data(), value.size()))));
}
const char* dtype(DType value) {
    switch (value) {
    case DType::BF16: return "bf16";
    case DType::FP32: return "fp32";
    case DType::I32: return "i32";
    case DType::U8: return "u8";
    case DType::I64: return "i64";
    case DType::I8: return "i8";
    case DType::FP16: return "fp16";
    case DType::FP8_E4M3FN: return "fp8_e4m3fn";
    }
    throw std::logic_error("unknown native dtype");
}
const char* kv_type(KvCacheStorage value) {
    switch (value) {
    case KvCacheStorage::BFloat16: return "bf16";
    case KvCacheStorage::Int8Group64: return "int8_group64";
    case KvCacheStorage::Fp8E4M3Row256: return "fp8_e4m3_row256";
    case KvCacheStorage::Nvfp4Group16:
    case KvCacheStorage::Fp8KeyNvfp4Value:
        throw Refusal{DurablePrepareState::PolicyDisabled, DurableReason::UnsupportedCodec};
    }
    throw std::logic_error("unknown native KV dtype");
}
const DurableNativeLayout& layout(ProgramImpl& p) {
    if (p.checkpoint_layout_cache) return *p.checkpoint_layout_cache;
    DurableNativeLayout result;
    result.continuation_codec = "qwen3_5-checkpoint-set/1";
    const auto& schema = p.state_images->storage().layout();
    Json fields = Json::array();
    for (const auto& field : schema.fields)
        fields.push_back({{"dtype", dtype(field.tensor.dtype)}, {"shape", field.tensor.shape},
            {"offset", field.tensor.region.offset}, {"bytes", field.tensor.region.bytes},
            {"alignment", field.tensor.region.alignment}, {"layers", field.layers},
            {"layer_stride", field.layer_stride}, {"slot_axis", field.slot_axis}});
    result.state_bytes = schema.image_stride;
    result.state_layout_sha256 = digest(Json{{"bytes", schema.image_stride}, {"fields", fields}}.dump());
    for (const auto& descriptor : p.kv_pools) {
        const auto& geometry = *descriptor.geometry;
        Json planes = Json::array();
        for (const auto& plane : geometry.planes)
            planes.push_back({{"dtype", dtype(plane.dtype)}, {"leading_extent", plane.leading_extent},
                {"head_extent", plane.head_extent}, {"alignment", plane.alignment},
                {"tokens_per_row", plane.tokens_per_row}});
        result.pools.push_back({.pool_id = descriptor.id.index, .name = std::string(descriptor.label),
            .layout_sha256 = digest(Json{{"planes", planes}, {"page_tokens", geometry.page_tokens},
                {"device_order", geometry.device_plane_order == PagedKVPlaneOrder::PageMajor ?
                    "page_major" : "head_major"}, {"host_stride", descriptor.host_page_stride}}.dump()),
            .storage_type = kv_type(p.kv_storage),
            .layers = static_cast<std::uint32_t>(geometry.planes.size() /
                (p.kv_storage == KvCacheStorage::BFloat16 ? 2U : 4U)),
            .tokens_per_page = geometry.page_tokens,
            .kv_heads = static_cast<std::uint32_t>(geometry.planes.front().head_extent),
            .head_dim = static_cast<std::uint32_t>(geometry.planes.front().leading_extent),
            .page_bytes = descriptor.device_page_bytes,
            .host_serialization_stride = descriptor.host_page_stride});
    }
    p.checkpoint_layout_cache = std::move(result);
    return *p.checkpoint_layout_cache;
}
std::string hex64(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t i = 0; i < 16; ++i) text[15 - i] = digits[(value >> (i * 4)) & 15];
    return text;
}
std::uint64_t from_hex(std::string_view value) {
    std::uint64_t result = 0;
    for (char c : value) result = (result << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
    return result;
}
DurableIdentity identity(const detail::PreparedCaptureBacking& backing, std::uint32_t frontier) {
    DurableIdentity result;
    const auto& exact = backing.prefix_identity;
    check(frontier <= exact.size() && frontier <= backing.ledger.size());
    result.ledger.assign(backing.ledger.begin(), backing.ledger.begin() + frontier);
    result.token_types.assign(exact.token_types().begin(), exact.token_types().begin() + frontier);
    for (std::size_t axis = 0; axis < 3; ++axis)
        result.positions[axis].assign(exact.positions(axis).begin(), exact.positions(axis).begin() + frontier);
    result.execution_frontiers.assign(exact.execution_frontiers().begin(),
        std::upper_bound(exact.execution_frontiers().begin(), exact.execution_frontiers().end(), frontier));
    result.shortlist_tag = backing.digests.at(frontier);
    for (const auto& item : exact.vision_items()) {
        check(!item.token_spans.empty());
        if (item.token_spans.front().begin >= frontier) break;
        check(item.token_spans.back().begin + item.token_spans.back().count <= frontier);
        DurableVisionIdentity out{.modality = item.modality == qwen::Modality::Image ? "image" : "video",
            .temporal = item.grid.temporal, .height = item.grid.height, .width = item.grid.width,
            .patch_begin = item.patch_begin, .patch_count = item.patch_count,
            .content_sha256 = crypto::sha256_hex(item.content_digest)};
        for (double timestamp : item.timestamps) out.timestamps_bits.push_back(hex64(std::bit_cast<std::uint64_t>(timestamp)));
        for (const auto& span : item.token_spans) {
            check(span.begin <= UINT32_MAX && span.count <= UINT32_MAX);
            out.token_spans.push_back({static_cast<std::uint32_t>(span.begin), static_cast<std::uint32_t>(span.count)});
        }
        result.vision_items.push_back(std::move(out));
    }
    return result;
}
std::shared_ptr<const detail::PreparedCaptureBacking>
restore_identity(const DurableIdentity& saved, const ProgramImpl& p) {
    check(saved.ledger.size() <= p.capacity);
    const auto vocabulary = p.parameters.model.resources().public_token_count;
    for (auto token : saved.ledger) check(token >= 0 && static_cast<std::uint64_t>(token) < vocabulary);
    qwen::PreparedPromptData prompt;
    prompt.token_ids = saved.ledger;
    prompt.token_types = saved.token_types;
    for (const auto& axis : saved.positions) prompt.positions.insert(prompt.positions.end(), axis.begin(), axis.end());
    prompt.identity.rewrite_execution_frontiers = saved.execution_frontiers;
    for (const auto& item : saved.vision_items) {
        qwen::VisionItem out{.modality = item.modality == "image" ? qwen::Modality::Image : qwen::Modality::Video,
            .grid = {item.temporal, item.height, item.width}, .patch_begin = item.patch_begin,
            .patch_count = item.patch_count};
        for (std::size_t i = 0; i < out.content_digest.size(); ++i)
            out.content_digest[i] = from_hex(std::string_view(item.content_sha256).substr(i * 2, 2));
        for (const auto& stamp : item.timestamps_bits) out.timestamps.push_back(std::bit_cast<double>(from_hex(stamp)));
        for (const auto& span : item.token_spans) out.token_spans.push_back({span.begin, span.count});
        check(item.patch_count <= UINT64_MAX - prompt.prepare.raw_patches);
        prompt.prepare.raw_patches += item.patch_count;
        for (const auto& span : item.token_spans) {
            check(span.count <= UINT64_MAX - prompt.prepare.vision_tokens);
            prompt.prepare.vision_tokens += span.count;
        }
        prompt.vision_items.push_back(std::move(out));
    }
    if (!prompt.vision_items.empty()) {
        if (!p.vision_enabled || !p.parameters.model.config().vision)
            throw Refusal{DurablePrepareState::IdentityMismatch, DurableReason::FingerprintMismatch};
        (void)qwen::plan_vision_control(prompt, *p.parameters.model.config().vision);
    }
    auto backing = std::make_shared<detail::PreparedCaptureBacking>();
    backing->prefix_identity.assign(prompt);
    backing->digests.assign(prompt);
    check(backing->digests.at(saved.ledger.size()) == saved.shortlist_tag);
    backing->ledger = std::move(prompt.token_ids);
    return backing;
}
DurablePointRole role(CheckpointRole value) {
    switch (value) {
    case CheckpointRole::InputReplay: return DurablePointRole::InputReplay;
    case CheckpointRole::LongAnchor: return DurablePointRole::LongAnchor;
    case CheckpointRole::Continuation: return DurablePointRole::Continuation;
    case CheckpointRole::SharedPrefix: break;
    }
    throw Refusal{DurablePrepareState::PolicyDisabled, DurableReason::UnsupportedCodec};
}
CheckpointRole role(DurablePointRole value) {
    switch (value) {
    case DurablePointRole::InputReplay: return CheckpointRole::InputReplay;
    case DurablePointRole::LongAnchor: return CheckpointRole::LongAnchor;
    case DurablePointRole::Continuation: return CheckpointRole::Continuation;
    }
    throw std::invalid_argument("invalid checkpoint role");
}
void settled(const DurablePoint& point, const ProgramImpl& p) {
    if (point.origin != DurablePointOrigin::Semantic)
        throw Refusal{DurablePrepareState::IdentityMismatch, DurableReason::UnsupportedCodec};
    check(point.frontier <= p.capacity && point.text_kv_frontier == point.frontier);
    const bool backend = p.backend_kv_pages != nullptr;
    check(point.histories.size() == (backend ? 2U : 1U));
    if (!backend) check(point.backend_frontier == 0);
    if (is_masked_draft_backend(p.speculative_backend))
        check(point.dflash_context_frontier == point.frontier);
    else check(point.dflash_context_frontier <= point.frontier);
}

struct ImportMetadata {
    DurableCheckpointStats stats;
    std::uint64_t resident_bytes = 0;
    std::vector<std::shared_ptr<const detail::PreparedCaptureBacking>> identities;
};
ImportMetadata validate_import_metadata(ProgramImpl& p, const DurableCheckpointSet& set,
    const ContinuationCheckpointReader& reader, const DurableLimits& limits) {
    codec::validate(set, limits);
    const auto& native = layout(p);
    if (set.architecture != "qwen3_5" || set.native_codec != native.continuation_codec ||
        set.state_layout_sha256 != native.state_layout_sha256 || set.pools.size() != native.pools.size())
        throw Refusal{DurablePrepareState::IdentityMismatch, DurableReason::FingerprintMismatch};
    for (std::size_t i = 0; i < set.pools.size(); ++i)
        if (set.pools[i].layout != native.pools[i])
            throw Refusal{DurablePrepareState::IdentityMismatch, DurableReason::FingerprintMismatch};
    for (const auto& point : set.points) settled(point, p);
    for (const auto& state : set.states) check(state.bytes == native.state_bytes);
    ImportMetadata metadata;
    for (const auto& file : set.payloads) {
        const auto descriptor = reader.file_descriptor(file.path);
        check(descriptor && descriptor->path == file.path && descriptor->bytes == file.bytes &&
            descriptor->sha256 == file.sha256 && descriptor->alignment == file.alignment,
            DurableReason::PayloadDigestMismatch);
        check(file.bytes <= UINT64_MAX - metadata.resident_bytes);
        metadata.resident_bytes += file.bytes;
    }
    const auto index_file = reader.file_descriptor("engine/index.cbor");
    check(index_file && index_file->alignment == 1 &&
          index_file->bytes == codec::encode(set).size(), DurableReason::PayloadSizeMismatch);
    check(index_file->bytes <= UINT64_MAX - metadata.resident_bytes);
    metadata.stats.payload_bytes = metadata.resident_bytes + index_file->bytes;
    metadata.stats.frontier_tokens = metadata.stats.restored_tokens = set.points[set.endpoint].frontier;
    metadata.stats.points = set.points.size();
    metadata.stats.states = set.states.size();
    for (const auto& pool : set.pools) metadata.stats.pages += pool.pages.size();
    metadata.identities.reserve(set.identities.size());
    for (const auto& saved : set.identities) metadata.identities.push_back(restore_identity(saved, p));
    return metadata;
}

struct CopyRows {
    const std::byte* source = nullptr;
    std::size_t destination = 0, source_pitch = 0, destination_pitch = 0, width = 0, rows = 0;
    bool device = false;
};
struct ExportRecord {
    std::uint32_t payload = 0;
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
    std::vector<CopyRows> copies;
};
struct Stream {
    cudaStream_t value = nullptr;
    Stream() { CUDA_CHECK(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking)); }
    ~Stream() { if (value) { (void)cudaStreamSynchronize(value); (void)cudaStreamDestroy(value); } }
};
} // namespace

struct ExportLease::Impl {
    DurablePrepareState state = DurablePrepareState::Prepared;
    DurableReason reason = DurableReason::None;
    DurableCheckpointStats stats;
    DurableCheckpointSet index;
    DurableLimits limits;
    Device device;
    std::size_t staging = 0;
    bool written = false;
    bool consumed = false;
    std::vector<detail::Checkpoints::Lease> checkpoints;
    std::vector<context::StateImageStore::ReadLease> states;
    std::vector<context::LogicalKVPageStore::ReadLease> pages;
    std::vector<ExportRecord> records;
};
ExportLease::ExportLease(std::unique_ptr<Impl> value) noexcept : impl_(std::move(value)) {}
ExportLease::ExportLease(ExportLease&&) noexcept = default;
ExportLease& ExportLease::operator=(ExportLease&&) noexcept = default;
ExportLease::~ExportLease() = default;
DurablePrepareState ExportLease::state() const noexcept { return impl_ ? impl_->state : DurablePrepareState::PolicyDisabled; }
DurableReason ExportLease::reason() const noexcept { return impl_ ? impl_->reason : DurableReason::Cancelled; }
DurableCheckpointStats ExportLease::stats() const noexcept { return impl_ ? impl_->stats : DurableCheckpointStats{}; }
const DurableCheckpointSet& ExportLease::index() const {
    if (!impl_ || !impl_->written) throw std::logic_error("checkpoint index requires completed export");
    return impl_->index;
}
DurableExportResult ExportLease::write(ContinuationCheckpointWriter& writer) {
    DurableExportResult result{.reason = reason(), .stats = stats()};
    if (state() != DurablePrepareState::Prepared) return result;
    if (impl_->consumed) throw std::logic_error("checkpoint export already consumed");
    auto& out = *impl_;
    out.consumed = true;
    CudaDeviceScope scope(out.device);
    PinnedHostBuffer staging(out.staging);
    Stream stream;
    try {
        std::size_t next_record = 0;
        for (std::size_t file = 0; file < out.index.payloads.size(); ++file) {
            auto& descriptor = out.index.payloads[file];
            crypto::Sha256 hash;
            for (; next_record < out.records.size() && out.records[next_record].payload == file; ++next_record) {
                const auto& record = out.records[next_record];
                std::memset(staging.data(), 0, record.bytes); // Never serialize holes or uncommitted KV.
                for (const auto& copy : record.copies) {
                    auto* destination = static_cast<std::byte*>(staging.data()) + copy.destination;
                    if (copy.device) {
                        CUDA_CHECK(cudaMemcpy2DAsync(destination, copy.destination_pitch, copy.source,
                            copy.source_pitch, copy.width, copy.rows, cudaMemcpyDeviceToHost, stream.value));
                    } else {
                        for (std::size_t row = 0; row < copy.rows; ++row)
                            std::memcpy(destination + row * copy.destination_pitch,
                                        copy.source + row * copy.source_pitch, copy.width);
                    }
                }
                CUDA_CHECK(cudaStreamSynchronize(stream.value));
                const std::span<const std::byte> bytes(static_cast<const std::byte*>(staging.data()), record.bytes);
                hash.update(bytes);
                if (!writer.write_file(descriptor.path, record.offset, descriptor.bytes, bytes, descriptor.alignment))
                    return {.reason = DurableReason::IoUnavailable, .stats = out.stats};
            }
            descriptor.sha256 = crypto::sha256_hex(hash.finish());
        }
        codec::validate(out.index, out.limits);
        const auto encoded = codec::encode(out.index);
        if (!writer.write_file("engine/index.cbor", 0, encoded.size(), std::as_bytes(std::span(encoded)), 1) ||
            !writer.finish())
            return {.reason = DurableReason::IoUnavailable, .stats = out.stats};
        out.written = true;
        return {.state = DurableExportState::Exported, .reason = DurableReason::None, .stats = out.stats};
    } catch (...) {
        (void)cudaStreamSynchronize(stream.value);
        throw;
    }
}

DurableNativeLayout Program::checkpoint_layout() const { return layout(*impl_); }
DurableCheckpointSet Program::decode_checkpoint_index(std::span<const std::byte> bytes,
                                                       const DurableLimits& limits) {
    return codec::decode(bytes, limits);
}

ExportLease Program::prepare_checkpoint_export(std::span<const CheckpointHandle> handles,
                                               const DurableLimits& limits) {
    auto out = std::make_unique<ExportLease::Impl>();
    out->limits = limits;
    auto& p = *impl_;
    try {
        if (!p.context_cache.enabled) throw Refusal{DurablePrepareState::PolicyDisabled, DurableReason::CacheDisabled};
        if (p.has_context_operation() || p.pending_transaction_)
            throw Refusal{DurablePrepareState::CapacityRefused, DurableReason::TransactionBusy};
        check(!handles.empty() && handles.size() <= std::min(limits.max_points, kDurableMaxPoints));
        const auto& native = layout(p);
        out->device = p.device.device();
        out->index.architecture = "qwen3_5";
        out->index.native_codec = native.continuation_codec;
        out->index.state_layout_sha256 = native.state_layout_sha256;
        for (const auto& pool : native.pools) out->index.pools.push_back({.layout = pool});
        std::vector<StateImageHandle> images;
        std::vector<const detail::PreparedCaptureBacking*> identities;
        std::vector<std::uint32_t> identity_frontiers;
        struct HistorySource { context::KVHistoryHandle handle; std::uint32_t pool; };
        std::vector<HistorySource> histories;
        for (auto handle : handles) {
            check(p.valid_checkpoint(handle));
            check(std::none_of(out->checkpoints.begin(), out->checkpoints.end(), [&](const auto& lease) {
                return lease.handle() == handle;
            }));
            out->checkpoints.push_back(p.checkpoints.lease(handle));
            const auto& record = p.checkpoint(handle);
            const auto& metadata = record.continuation().metadata;
            DurablePoint point{.role = role(record.role), .frontier = record.frontier(),
                .text_kv_frontier = metadata.text_kv_valid, .backend_frontier = metadata.backend_kv_valid,
                .rope_delta = metadata.rope_delta, .tail_hidden_valid = metadata.tail_hidden_valid,
                .dflash_context_frontier = metadata.dflash_context_frontier};
            if (point.role == DurablePointRole::Continuation) out->index.endpoint = out->index.points.size();
            const auto image = record.continuation().image;
            auto state = std::find(images.begin(), images.end(), image);
            point.state = std::distance(images.begin(), state);
            if (state == images.end()) images.push_back(image);
            const auto* identity_source = &record.snapshot.identity();
            auto found = std::find(identities.begin(), identities.end(), identity_source);
            point.identity = std::distance(identities.begin(), found);
            if (found == identities.end()) {
                identities.push_back(identity_source);
                identity_frontiers.push_back(point.frontier);
            } else identity_frontiers[point.identity] = std::max(identity_frontiers[point.identity], point.frontier);
            for (std::size_t pool = 0; pool < record.snapshot.prefixes().size(); ++pool) {
                const auto& prefix = record.snapshot.prefix(pool);
                auto history = std::find_if(histories.begin(), histories.end(), [&](const auto& h) {
                    return h.pool == pool && h.handle == prefix.history();
                });
                const auto id = static_cast<std::uint32_t>(std::distance(histories.begin(), history));
                if (history == histories.end()) {
                    histories.push_back({prefix.history(), static_cast<std::uint32_t>(pool)});
                    out->index.histories.push_back({.pool = static_cast<std::uint32_t>(pool),
                                                   .committed_tokens = prefix.frontier()});
                } else out->index.histories[id].committed_tokens =
                    std::max(out->index.histories[id].committed_tokens, prefix.frontier());
                point.histories.push_back({id, prefix.frontier()});
            }
            settled(point, p);
            out->index.points.push_back(std::move(point));
        }
        std::uint64_t metadata_bytes = 0;
        for (std::size_t i = 0; i < identities.size(); ++i) {
            metadata_bytes += static_cast<std::uint64_t>(identity_frontiers[i]) * 24;
            capacity(metadata_bytes <= limits.staging_bytes);
            out->index.identities.push_back(identity(*identities[i], identity_frontiers[i]));
        }
        std::array<std::vector<context::LogicalKVPageHandle>, 2> page_handles;
        std::array<std::unordered_map<std::uint32_t, std::uint32_t>, 2> page_ids;
        const auto history_store = [&](std::uint32_t pool) -> context::KVHistoryStore& {
            return pool == 0 ? *p.text_kv_histories : *p.backend_kv_histories;
        };
        for (std::size_t i = 0; i < histories.size(); ++i) {
            auto& saved = out->index.histories[i];
            auto& store = history_store(saved.pool);
            auto& pages = store.pages();
            check(saved.committed_tokens <= store.committed_frontier(histories[i].handle));
            for (std::uint32_t page = 0; page < (saved.committed_tokens + 63U) / 64U; ++page) {
                const auto handle = store.logical_page(histories[i].handle, page);
                const auto needed = std::min(64U, saved.committed_tokens - page * 64U);
                auto [entry, inserted] = page_ids[saved.pool].emplace(pages.descriptor_index(handle), page_handles[saved.pool].size());
                const auto id = entry->second;
                if (inserted) {
                    page_handles[saved.pool].push_back(handle);
                    out->index.pools[saved.pool].pages.push_back({.committed_tokens = needed});
                } else out->index.pools[saved.pool].pages[id].committed_tokens =
                    std::max(out->index.pools[saved.pool].pages[id].committed_tokens, needed);
                saved.pages.push_back(id);
            }
        }
        // Retire native producers once. Off-lock copies never resolve a live handle again.
        p.device.synchronize();
        CUDA_CHECK(cudaStreamSynchronize(p.device.transfer_stream()));
        for (std::size_t pool = 0; pool < out->index.pools.size(); ++pool) {
            auto& saved = out->index.pools[pool];
            if (saved.pages.empty()) continue;
            auto& pages = history_store(pool).pages();
            auto& physical = pages.physical_pool();
            const auto host_layout = plan_host_kv_page_layout(physical.geometry());
            const auto payload = static_cast<std::uint32_t>(out->index.payloads.size());
            out->index.payloads.push_back({"engine/kv/" + std::to_string(pool) + ".bin",
                host_layout.page_stride * saved.pages.size(), std::string(64, '0'), 256});
            for (std::size_t i = 0; i < saved.pages.size(); ++i) {
                auto& page = saved.pages[i];
                page.payload = payload;
                page.offset = i * host_layout.page_stride;
                page.bytes = host_layout.page_stride;
                const auto handle = page_handles[pool][i];
                const bool host = pages.host_covers(handle, page.committed_tokens);
                out->pages.push_back(host ? pages.read_host(handle, page.committed_tokens) :
                                            pages.read_device(handle, page.committed_tokens));
                const std::byte* host_data = nullptr;
                if (host) {
                    const auto replica = pages.host_replica(handle);
                    host_data = p.host_kv_extents->view(replica.extent).subview(replica.page_offset, 1).data();
                }
                ExportRecord record{payload, page.offset, host_layout.page_stride};
                for (std::size_t plane = 0; plane < physical.plane_count(); ++plane) {
                    const auto& geometry = physical.geometry().planes[plane];
                    const auto& packed = host_layout.planes[plane];
                    if (geometry.tokens_per_row != 1)
                        throw Refusal{DurablePrepareState::PolicyDisabled, DurableReason::UnsupportedCodec};
                    const auto width = static_cast<std::size_t>(page.committed_tokens / geometry.tokens_per_row) *
                        geometry.leading_extent * dtype_size(geometry.dtype);
                    if (!width) continue;
                    const auto source = host ? Tensor{} : physical.page_plane(pages.physical(handle), plane);
                    record.copies.push_back({host ? host_data + packed.offset : static_cast<const std::byte*>(source.data),
                        packed.offset, host ? packed.head_payload_bytes : static_cast<std::size_t>(source.nb[2 +
                            (physical.geometry().device_plane_order == PagedKVPlaneOrder::HeadMajor)]),
                        packed.head_payload_bytes, width, static_cast<std::size_t>(geometry.head_extent), !host});
                }
                out->staging = std::max(out->staging, record.bytes);
                out->records.push_back(std::move(record));
            }
        }
        const auto& schema = p.state_images->storage().layout();
        for (auto image : images) {
            const auto payload = static_cast<std::uint32_t>(out->index.payloads.size());
            const auto id = out->index.states.size();
            out->index.payloads.push_back({"engine/state/" + std::to_string(id) + ".bin",
                                          native.state_bytes, std::string(64, '0'), 256});
            out->index.states.push_back({native.state_layout_sha256, payload, 0, native.state_bytes});
            out->states.push_back(p.state_store->read(image));
            const bool host = p.state_store->host_resident(image);
            const auto source = host ? out->states.back().host_view() :
                ConstMemorySpan(p.state_images->storage().image(out->states.back().device_slot()));
            ExportRecord record{payload, 0, static_cast<std::size_t>(native.state_bytes)};
            for (const auto& field : schema.fields)
                record.copies.push_back({static_cast<const std::byte*>(source.data) + field.tensor.region.offset,
                    field.tensor.region.offset, field.layer_stride, field.layer_stride,
                    field.tensor.region.bytes, field.layers, !host});
            out->staging = std::max(out->staging, record.bytes);
            out->records.push_back(std::move(record));
        }
        capacity(out->staging <= limits.staging_bytes);
        // Dense payload IDs are remapped once to the contract's lexicographic path order.
        std::vector<std::uint32_t> order(out->index.payloads.size()), remap(order.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return out->index.payloads[a].path < out->index.payloads[b].path; });
        std::vector<DurablePayload> sorted;
        sorted.reserve(order.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) { remap[order[i]] = i; sorted.push_back(std::move(out->index.payloads[order[i]])); }
        out->index.payloads = std::move(sorted);
        for (auto& pool : out->index.pools) for (auto& page : pool.pages) page.payload = remap[page.payload];
        for (auto& state : out->index.states) state.payload = remap[state.payload];
        for (auto& record : out->records) record.payload = remap[record.payload];
        std::sort(out->records.begin(), out->records.end(), [](const auto& a, const auto& b) {
            return std::tie(a.payload, a.offset) < std::tie(b.payload, b.offset);
        });
        codec::validate(out->index, limits);
        out->stats.frontier_tokens = out->stats.restored_tokens = out->index.points[out->index.endpoint].frontier;
        out->stats.points = out->index.points.size();
        out->stats.states = out->index.states.size();
        for (const auto& pool : out->index.pools) out->stats.pages += pool.pages.size();
        for (const auto& file : out->index.payloads) {
            check(file.bytes <= UINT64_MAX - out->stats.payload_bytes);
            out->stats.payload_bytes += file.bytes;
        }
        // Acknowledged durability must be restorable into an otherwise empty matching arena.
        capacity(p.host_context_arena && out->stats.payload_bytes <= p.host_context_arena->capacity_bytes() &&
                 images.size() <= p.state_store->capacity() && histories.size() <=
                     p.text_kv_histories->capacity() + (p.backend_kv_histories ? p.backend_kv_histories->capacity() : 0));
        out->stats.payload_bytes += codec::encode(out->index).size();
    } catch (const Refusal& failure) {
        out = std::make_unique<ExportLease::Impl>(); out->state = failure.state; out->reason = failure.reason;
    } catch (const std::invalid_argument&) {
        out = std::make_unique<ExportLease::Impl>(); out->state = DurablePrepareState::Corrupt; out->reason = DurableReason::InvalidIndex;
    } catch (const std::bad_alloc&) {
        out = std::make_unique<ExportLease::Impl>(); out->state = DurablePrepareState::CapacityRefused; out->reason = DurableReason::Capacity;
    }
    return ExportLease(std::move(out));
}

struct ImportReservation::Impl {
    ProgramImpl* owner = nullptr;
    DurablePrepareState state = DurablePrepareState::Prepared;
    DurableReason reason = DurableReason::None;
    DurableCheckpointStats stats;
    std::vector<CheckpointHandle> slots;
    std::vector<context::StateImageStore::Destination> images;
    std::vector<context::HostKVExtentStore::Import> pages;
    std::vector<context::KVHistory> histories;
    std::vector<detail::CheckpointState> records;
    ~Impl() {
        records.clear();
        histories.clear();
        pages.clear();
        images.clear();
        if (owner) for (auto slot : slots) owner->checkpoints.cancel(slot);
    }
};
ImportReservation::ImportReservation(std::unique_ptr<Impl> value) noexcept : impl_(std::move(value)) {}
ImportReservation::ImportReservation(ImportReservation&&) noexcept = default;
ImportReservation& ImportReservation::operator=(ImportReservation&&) noexcept = default;
ImportReservation::~ImportReservation() = default;
DurablePrepareState ImportReservation::state() const noexcept { return impl_ ? impl_->state : DurablePrepareState::PolicyDisabled; }
DurableReason ImportReservation::reason() const noexcept { return impl_ ? impl_->reason : DurableReason::Cancelled; }
DurableCheckpointStats ImportReservation::stats() const noexcept { return impl_ ? impl_->stats : DurableCheckpointStats{}; }
bool ImportReservation::valid() const noexcept { return impl_ && impl_->owner && impl_->state == DurablePrepareState::Prepared; }

CheckpointValidation Program::validate_checkpoint_import(const DurableCheckpointSet& set,
    const ContinuationCheckpointReader& reader, const DurableLimits& limits) const {
    CheckpointValidation result;
    try {
        const auto metadata = validate_import_metadata(*impl_, set, reader, limits);
        result.stats = metadata.stats;
        const auto bytes = static_cast<std::size_t>(std::min({limits.staging_bytes,
            metadata.resident_bytes, std::uint64_t{4U << 20}}));
        check(bytes != 0);
        auto scratch = std::make_unique_for_overwrite<std::byte[]>(bytes);
        for (const auto& file : set.payloads) {
            for (std::uint64_t offset = 0; offset < file.bytes;) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, file.bytes - offset));
                check_read(reader.read_file(file.path, offset, {scratch.get(), count}), reader);
                offset += count;
            }
        }
        check_read(reader.verify_complete(), reader);
        result.state = CheckpointValidationState::Validated;
        result.reason = DurableReason::None;
    } catch (const Refusal& failure) {
        result.state = failure.state == DurablePrepareState::Unavailable ? CheckpointValidationState::Unavailable :
            failure.state == DurablePrepareState::IdentityMismatch || failure.state == DurablePrepareState::PolicyDisabled
                ? CheckpointValidationState::IdentityMismatch : CheckpointValidationState::Corrupt;
        result.reason = failure.reason;
    } catch (const std::invalid_argument&) {
        result.reason = DurableReason::InvalidIndex;
    } catch (const std::overflow_error&) {
        result.reason = DurableReason::InvalidIndex;
    }
    return result;
}

ImportReservation Program::prepare_checkpoint_import(const DurableCheckpointSet& set,
    const ContinuationCheckpointReader& reader, const DurableLimits& limits) {
    auto out = std::make_unique<ImportReservation::Impl>();
    auto& p = *impl_;
    out->owner = &p;
    try {
        if (!p.context_cache.enabled) throw Refusal{DurablePrepareState::PolicyDisabled, DurableReason::CacheDisabled};
        if (p.has_context_operation() || p.pending_transaction_)
            throw Refusal{DurablePrepareState::CapacityRefused, DurableReason::TransactionBusy};
        auto metadata = validate_import_metadata(p, set, reader, limits);
        out->stats = metadata.stats;
        capacity(p.host_context_arena && p.host_kv_extents &&
                 metadata.resident_bytes <= p.host_context_arena->free_bytes());
        auto& identities = metadata.identities;
        out->slots.reserve(set.points.size()); out->records.reserve(set.points.size());
        out->images.reserve(set.states.size()); out->pages.reserve(set.pools.size());
        out->histories.reserve(set.histories.size());
        for (std::size_t i = 0; i < set.points.size(); ++i) {
            const auto slot = p.reserve_checkpoint(); capacity(slot.has_value()); out->slots.push_back(*slot);
        }
        std::vector<StateImageHandle> images;
        for (std::size_t i = 0; i < set.states.size(); ++i) {
            const auto image = p.state_store->reserve_host_destination(); capacity(image.has_value());
            out->images.emplace_back(*p.state_store, *image); images.push_back(*image);
        }
        std::array<std::span<const context::LogicalKVPageHandle>, 2> page_handles;
        std::array<std::optional<HostKVAllocationView>, 2> page_views;
        for (std::size_t pool = 0; pool < set.pools.size(); ++pool) {
            if (set.pools[pool].pages.empty()) continue;
            auto& pages = pool == 0 ? *p.text_kv_pages : *p.backend_kv_pages;
            std::vector<std::uint32_t> coverage;
            for (const auto& page : set.pools[pool].pages) coverage.push_back(page.committed_tokens);
            auto imported = p.host_kv_extents->prepare_import(pages, coverage); capacity(imported.has_value());
            out->pages.push_back(std::move(*imported));
            page_handles[pool] = out->pages.back().pages();
            page_views[pool] = out->pages.back().writable_view();
        }
        struct Read { std::uint32_t file; std::uint64_t offset; std::span<std::byte> destination; };
        std::vector<Read> reads;
        reads.reserve(out->stats.states + out->stats.pages);
        for (std::size_t i = 0; i < set.states.size(); ++i) {
            const auto& saved = set.states[i];
            const auto destination = p.state_store->writable_host_view(images[i]);
            reads.push_back({saved.payload, saved.offset, {static_cast<std::byte*>(destination.data), destination.bytes}});
        }
        for (std::size_t pool = 0; pool < set.pools.size(); ++pool)
            for (std::size_t i = 0; i < set.pools[pool].pages.size(); ++i) {
                const auto& saved = set.pools[pool].pages[i];
                auto view = page_views[pool]->subview(i, 1);
                reads.push_back({saved.payload, saved.offset, {view.data(), static_cast<std::size_t>(saved.bytes)}});
            }
        std::sort(reads.begin(), reads.end(), [](const auto& a, const auto& b) {
            return std::tie(a.file, a.offset) < std::tie(b.file, b.offset);
        });
        for (const auto& read : reads)
            check_read(reader.read_file(set.payloads[read.file].path, read.offset, read.destination), reader);
        check_read(reader.verify_complete(), reader);
        for (auto image : images) p.state_store->complete_host_initialization(image);
        for (auto& pages : out->pages) pages.publish();
        for (const auto& saved : set.histories) {
            auto& store = saved.pool == 0 ? *p.text_kv_histories : *p.backend_kv_histories;
            std::vector<context::LogicalKVPageHandle> members;
            for (auto id : saved.pages) members.push_back(page_handles[saved.pool][id]);
            auto history = store.import_history(members, saved.committed_tokens); capacity(history.has_value());
            out->histories.push_back(std::move(*history));
        }
        std::vector<bool> adopted(images.size());
        const auto tag = static_cast<std::uint32_t>(p.speculative_backend) |
            (static_cast<std::uint32_t>(p.proposal_head) << 8U) | (static_cast<std::uint32_t>(p.kv_storage) << 16U);
        for (const auto& point : set.points) {
            std::vector<context::KVPrefixRef> prefixes;
            for (const auto& prefix : point.histories) {
                auto& store = set.histories[prefix.history].pool == 0 ? *p.text_kv_histories : *p.backend_kv_histories;
                prefixes.push_back(store.retain_prefix(out->histories[prefix.history].handle(), prefix.frontier));
            }
            Continuation continuation{images[point.state], {.execution_frontier = point.frontier,
                .rope_delta = point.rope_delta, .text_kv_valid = point.text_kv_frontier,
                .backend_kv_valid = point.backend_frontier, .dflash_context_frontier = point.dflash_context_frontier,
                .tail_hidden_valid = point.tail_hidden_valid}};
            out->records.push_back({.snapshot = detail::Snapshot(*p.state_store, continuation,
                std::move(prefixes), identities[point.identity]), .role = role(point.role),
                .key = {identities[point.identity]->digests.at(point.frontier), point.frontier, tag}});
            if (!adopted[point.state]) { (void)out->images[point.state].release(); adopted[point.state] = true; }
        }
    } catch (const Refusal& failure) {
        out = std::make_unique<ImportReservation::Impl>(); out->state = failure.state; out->reason = failure.reason;
    } catch (const std::invalid_argument&) {
        out = std::make_unique<ImportReservation::Impl>(); out->state = DurablePrepareState::Corrupt; out->reason = DurableReason::InvalidIndex;
    } catch (const std::bad_alloc&) {
        out = std::make_unique<ImportReservation::Impl>(); out->state = DurablePrepareState::CapacityRefused; out->reason = DurableReason::Capacity;
    } catch (const std::overflow_error&) {
        out = std::make_unique<ImportReservation::Impl>(); out->state = DurablePrepareState::Corrupt; out->reason = DurableReason::InvalidIndex;
    }
    return ImportReservation(std::move(out));
}

std::vector<CheckpointHandle> Program::commit_import(ImportReservation&& prepared) {
    if (!prepared.valid() || prepared.impl_->owner != impl_.get())
        throw std::logic_error("checkpoint import is foreign, refused or consumed");
    auto& value = *prepared.impl_;
    if (value.slots.size() != value.records.size()) throw std::logic_error("checkpoint import is incomplete");
    for (std::size_t i = 0; i < value.slots.size(); ++i)
        impl_->checkpoints.publish(value.slots[i], std::move(value.records[i]));
    value.records.clear();
    value.owner = nullptr;
    auto handles = std::move(value.slots);
    prepared.impl_.reset();
    return handles;
}
} // namespace ninfer::models::qwen3_5
