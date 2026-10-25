// Local, single-request feasibility tracer. Not a product checkpoint format.
#include "models/qwen3_5/program/program_impl.h"
#include "models/common/context/work.h"
#include "models/common/qwen/frontend/digest.h"
#include "core/device.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ninfer::models::qwen3_5 {
void Program::trace_export_checkpoint(CheckpointHandle checkpoint) {
    impl_->trace_export_checkpoint(checkpoint);
}
std::optional<CheckpointHandle> Program::trace_import_checkpoint(const RequestBasePlan& base) {
    return impl_->trace_import_checkpoint(base);
}
namespace detail {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace frontend = qwen::frontend;
using Bytes = std::span<const std::uint8_t>;

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
Bytes bytes(const void* data, std::size_t size) {
    return {static_cast<const std::uint8_t*>(data), size};
}
std::string digest(Bytes data) { return frontend::sha256_hex(frontend::sha256(data)); }

#if !defined(_WIN32)
struct File {
    int fd = -1;
    explicit File(int value) : fd(value) { require(fd >= 0, "tracer file open failed"); }
    ~File() { if (fd >= 0) { ::close(fd); } }
    File(const File&) = delete;
};
void sync_dir(const fs::path& path) {
    File file(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    require(::fsync(file.fd) == 0, "tracer directory sync failed");
}
void save_file(const fs::path& path, Bytes data) {
    File file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(file.fd, data.data() + offset, data.size() - offset);
        require(count > 0, "tracer file write failed");
        offset += static_cast<std::size_t>(count);
    }
    require(::fsync(file.fd) == 0, "tracer file sync failed");
}
void read_into(const fs::path& path, std::span<std::uint8_t> destination) {
    File file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
    struct stat info{};
    require(::fstat(file.fd, &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) == destination.size(),
            "tracer file size/type mismatch");
    std::size_t offset = 0;
    while (offset < destination.size()) {
        const auto count = ::read(file.fd, destination.data() + offset, destination.size() - offset);
        require(count > 0, "tracer file read failed");
        offset += static_cast<std::size_t>(count);
    }
}
#else
void sync_dir(const fs::path&) { throw std::runtime_error("tracer is Linux-only"); }
void save_file(const fs::path&, Bytes) { throw std::runtime_error("tracer is Linux-only"); }
void read_into(const fs::path&, std::span<std::uint8_t>) {
    throw std::runtime_error("tracer is Linux-only");
}
#endif
std::vector<std::uint8_t> read_bounded(const fs::path& path, std::size_t maximum) {
    const auto size = fs::file_size(path);
    require(size <= maximum, "tracer file exceeds bound");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    read_into(path, out);
    return out;
}
std::string mac(std::string_view text) {
    const auto key = read_bounded(env_value("NINFER_TRACER_KEY_FILE"), 32);
    require(key.size() == 32, "tracer requires a separate 32-byte origin key");
    std::vector<std::uint8_t> inner(64 + text.size(), 0x36);
    std::array<std::uint8_t, 96> outer{};
    std::fill_n(outer.begin(), 64, 0x5c);
    for (std::size_t i = 0; i < key.size(); ++i) {
        inner[i] ^= key[i];
        outer[i] ^= key[i];
    }
    std::memcpy(inner.data() + 64, text.data(), text.size());
    const auto sum = frontend::sha256(inner);
    std::copy(sum.begin(), sum.end(), outer.begin() + 64);
    return digest(outer);
}
bool equal_mac(std::string_view a, std::string_view b) {
    if (a.size() != 64 || b.size() != 64) { return false; }
    unsigned difference = 0;
    for (std::size_t i = 0; i < 64; ++i) { difference |= a[i] ^ b[i]; }
    return difference == 0;
}
Json runtime_identity(const ProgramImpl& p) {
    const auto supplied = env_value("NINFER_TRACER_IDENTITY");
    require(!supplied.empty(), "tracer needs measured launcher identities");
    return {{"launcher", Json::parse(supplied)}, {"capacity", p.capacity},
            {"kv_capacity", p.kv_capacity}, {"max_concurrency", p.max_concurrency},
            {"prefill_chunk", p.prefill_chunk}, {"draft_window", p.draft_window},
            {"backend", static_cast<int>(p.speculative_backend)},
            {"kv_storage", static_cast<int>(p.kv_storage)},
            {"proposal_head", static_cast<int>(p.proposal_head)},
            {"graphs", p.use_cuda_graph}, {"state_bytes", p.state_images->storage().image_bytes()},
            {"kv_page_bytes", p.text_host_kv_page_stride}};
}
void check_mode(const ProgramImpl& p) {
    require(p.max_concurrency == 1 && p.kv_storage == KvCacheStorage::BFloat16 &&
            p.speculative_backend == SpeculativeBackend::DFlash2 && !p.backend_kv_pages &&
            p.host_kv_arena && p.context_cache.enabled &&
            !p.has_context_operation() && !p.pending_transaction_,
            "tracer requires idle-transaction single-lane BF16 DFlash2 with context cache");
}
} // namespace

void ProgramImpl::trace_export_checkpoint(CheckpointHandle handle) {
    if (env_value("NINFER_TRACER_MODE") != "export") { return; }
    const fs::path root(env_value("NINFER_TRACER_ROOT"));
    require(!root.empty(), "tracer root is missing");
    if (fs::exists(root)) { return; }
    const auto& record = checkpoint(handle); // owner + descriptor generation checked
    if (record.frontier() < 16384 || record.role != runtime::CheckpointRole::InputReplay) { return; }
    check_mode(*this);
    const auto& metadata = record.continuation().metadata;
    require(!record.backend_prefix() && record.backend_frontier() == 0 &&
            metadata.tail_hidden_valid && metadata.dflash_context_frontier == record.frontier() &&
            state_store->device_resident(record.continuation().image),
            "tracer checkpoint is not a complete device-resident DFlash2 input point");
    auto checkpoint_lease = checkpoints.lease(handle);
    const auto started = std::chrono::steady_clock::now();
    device.synchronize();
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
    const auto state_bytes = state_images->storage().image_bytes();
    require(state_images->dflash_local() != nullptr,
            "tracer requires draft ring in StateImage");
    auto exact = record.snapshot.identity().prefix_identity;
    exact.truncate(record.frontier());
    Json manifest{{"format", "ninfer-upstream-durable-tracer-1"},
                  {"runtime", runtime_identity(*this)}, {"frontier", record.frontier()},
                  {"identity_tag", record.key.identity_tag}, {"rope_delta", metadata.rope_delta},
                  {"identity", exact.trace_text_identity()},
                  {"tokens", std::vector<TokenId>(record.snapshot.identity().ledger.begin(),
                      record.snapshot.identity().ledger.begin() + record.frontier())},
                  {"files", Json::object()}};
    fs::path staging(root.string() + ".staging");
    require(fs::create_directory(staging), "tracer staging already exists; preserve failed attempt");
    fs::permissions(staging, fs::perms::owner_all, fs::perm_options::replace);
    const auto save = [&](const std::string& name, Bytes data) {
        save_file(staging / name, data);
        manifest["files"][name] = {{"bytes", data.size()}, {"sha256", digest(data)}};
    };
    PinnedHostBuffer state_buffer(state_bytes);
    {
        auto lease = state_store->read(record.continuation().image);
        try {
            state_images->storage().copy_to_host(lease.device_slot(), state_buffer.span(),
                                                device.transfer_view());
            CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
        } catch (...) {
            (void)cudaStreamSynchronize(device.transfer_stream());
            throw;
        }
        save("state.bin", bytes(state_buffer.data(), state_bytes));
    }
    auto& pages = *text_kv_pages;
    auto& pool = pages.physical_pool();
    const auto page_layout = plan_host_kv_page_layout(pool.geometry());
    auto staging_page = host_kv_arena->allocate(page_layout, 1);
    require(staging_page.has_value(), "tracer host page admission failed");
    const auto host = host_kv_arena->writable_view(*staging_page);
    const auto count = context::kv_pages_for_frontier(record.frontier());
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto logical = text_kv_histories->logical_page(record.text_prefix().history(), i);
        auto lease = pages.read_device(logical, std::min<std::uint32_t>(
            kPagedKVPageSize, record.frontier() - i * kPagedKVPageSize));
        const auto source = pages.physical(logical);
        try {
            pool.copy_to_host(std::span(&source, 1), host, device.transfer_stream());
            CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
        } catch (...) {
            (void)cudaStreamSynchronize(device.transfer_stream());
            throw;
        }
        save("page-" + std::to_string(i) + ".bin", bytes(host.data(), page_layout.page_stride));
    }
    const std::string text = manifest.dump();
    const std::string signature = mac(text);
    save_file(staging / "manifest.json", bytes(text.data(), text.size()));
    save_file(staging / "manifest.mac", bytes(signature.data(), signature.size()));
    sync_dir(staging);
    fs::rename(staging, root);
    sync_dir(root.parent_path());
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "durable-tracer export frontier=" << record.frontier() << " pages=" << count
              << " seconds=" << elapsed << " manifest_sha256=" << digest(bytes(text.data(), text.size()))
              << '\n';
}

std::optional<CheckpointHandle> ProgramImpl::trace_import_checkpoint(const RequestBasePlan& base) {
    if (env_value("NINFER_TRACER_MODE") != "import" || trace_import_attempted) { return std::nullopt; }
    trace_import_attempted = true;
    check_mode(*this);
    require(base.impl_ && !base.impl_->prompt->has_media() &&
            std::all_of(requests.begin(), requests.end(), [](const auto& r) {
                return r.lifecycle == Lifecycle::Empty;
            }), "tracer import requires empty Program and text request");
    const auto started = std::chrono::steady_clock::now();
    const fs::path root(env_value("NINFER_TRACER_ROOT"));
    const auto raw = read_bounded(root / "manifest.json", 16U << 20);
    const std::string text(raw.begin(), raw.end());
    const auto signature = read_bounded(root / "manifest.mac", 64);
    require(equal_mac(mac(text), std::string_view(reinterpret_cast<const char*>(signature.data()),
                                                 signature.size())), "tracer manifest MAC mismatch");
    const Json manifest = Json::parse(text);
    require(manifest.at("format") == "ninfer-upstream-durable-tracer-1" &&
            manifest.at("runtime") == runtime_identity(*this), "tracer runtime identity mismatch");
    const auto frontier = manifest.at("frontier").get<std::uint32_t>();
    require(frontier >= 16384 && frontier < capacity &&
            frontier <= base.impl_->prompt->token_ids.size(), "tracer frontier outside input");
    const auto tokens = manifest.at("tokens").get<std::vector<TokenId>>();
    require(tokens.size() == frontier && std::equal(tokens.begin(), tokens.end(),
            base.impl_->prompt->token_ids.begin()), "tracer prefix token mismatch");
    qwen::ResidentPrefixIdentity identity;
    identity.assign(*base.impl_->prompt);
    identity.truncate(frontier);
    require(manifest.at("identity") == identity.trace_text_identity() &&
            manifest.at("identity_tag") == base.impl_->prefix_identity_tag &&
            manifest.at("rope_delta") == base.impl_->prompt->rope_delta,
            "tracer exact prefix identity mismatch");
    const auto state_bytes = state_images->storage().image_bytes();
    auto& pages = *text_kv_pages;
    auto& pool = pages.physical_pool();
    const auto page_layout = plan_host_kv_page_layout(pool.geometry());
    const auto count = context::kv_pages_for_frontier(frontier);
    require(manifest.at("files").is_object() && manifest.at("files").size() == count + 1U,
            "tracer inventory mismatch");
    const auto load = [&](const std::string& name, std::span<std::uint8_t> destination) {
        const auto& description = manifest.at("files").at(name);
        require(description.at("bytes") == destination.size(), "tracer payload geometry mismatch");
        read_into(root / name, destination);
        require(description.at("sha256") == digest(destination), "tracer payload SHA256 mismatch");
    };
    PinnedHostBuffer state_buffer(state_bytes);
    load("state.bin", {static_cast<std::uint8_t*>(state_buffer.data()), state_bytes});
    auto staging_page = host_kv_arena->allocate(page_layout, 1);
    require(staging_page.has_value(), "tracer host staging admission failed");
    const auto host = host_kv_arena->writable_view(*staging_page);
    std::optional<CheckpointHandle> imported;
    try {
        const auto reservation = start_binding(base, runtime::LaneId{0}, SourceCandidate{}, nullptr,
                                                ExecutionUnitKind::Decode, 1);
        require(reservation.reserved, "tracer root binding admission failed");
        ContextProgress progress;
        for (unsigned phase = 0; phase < 3 && !progress.complete; ++phase) {
            CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
            progress = poll_context({});
        }
        require(progress.complete && progress.sequence.has_value(), "tracer root binding incomplete");
        auto& state = sequences[0];
        require(!state.pending_write.valid() &&
                state_store->role(state.continuation.image) == context::StateImageRole::ActiveMutable,
                "tracer destination state is not exclusively mutable");
        text_kv_histories->reserve_growth(state.kv->text.handle(),
            text_kv_histories->growth_pages_for_tokens(state.kv->text.handle(), frontier));
        ensure_sequence_kv_mapped(state, frontier);
        device.synchronize();
        state_images->storage().copy_from_host(state_buffer.span(),
            state_store->physical_slot(state.continuation.image), device.transfer_view());
        for (std::uint32_t i = 0; i < count; ++i) {
            load("page-" + std::to_string(i) + ".bin",
                 {reinterpret_cast<std::uint8_t*>(host.data()), page_layout.page_stride});
            const auto destination = text_kv_histories->physical_page(state.kv->text.handle(), i);
            pool.copy_from_host(HostKVAllocationConstView(host), std::span(&destination, 1),
                                device.transfer_stream());
            CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
        }
        CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
        commit_sequence_kv(state, frontier);
        state.ledger = tokens;
        state.prefix_identity = std::move(identity);
        state.prefix_digests = base.impl_->prefix_digests;
        state.prefix_digests.truncate(frontier);
        state.continuation.metadata.execution_frontier = state.ledger_frontier =
            state.continuation.metadata.text_kv_valid = frontier;
        state.continuation.metadata.dflash_context_frontier = frontier;
        state.continuation.metadata.tail_hidden_valid = true;
        imported = detach_checkpoint(state); // normal freeze/retain/deactivate, fresh generations
        require(imported.has_value() && valid_checkpoint(*imported), "tracer checkpoint publication failed");
        checkpoint(*imported).role = runtime::CheckpointRole::InputReplay;
        clear_lane(state, requests[0]);
        require(checkpoint_matches(*imported, base), "tracer imported checkpoint failed native matching");
    } catch (...) {
        device.synchronize();
        (void)cudaStreamSynchronize(device.transfer_stream());
        abort_context();
        if (requests[0].lifecycle != Lifecycle::Empty) { clear_lane(sequences[0], requests[0]); }
        if (imported) { (void)release_checkpoint(*imported); }
        throw;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "durable-tracer import frontier=" << frontier << " pages=" << count
              << " seconds=" << elapsed << " owner_checked=true generation=" << imported->generation
              << " manifest_sha256=" << digest(bytes(text.data(), text.size())) << '\n';
    return imported;
}
} // namespace detail
} // namespace ninfer::models::qwen3_5
