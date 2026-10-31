#pragma once

// DIAGNOSTIC BRANCH ONLY. Never merge into a release.
// NINFER_TRUECOLD_T1_DIR must name a fresh directory for each process.
// Captures the preregistered 3093-token T1 case without changing arithmetic or widths.
#include "models/qwen3_5/program/program_impl.h"
#include "core/sha256.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>

namespace ninfer::models::qwen3_5::detail::truecold_diagnostic {
inline const std::filesystem::path& root() {
    static const std::filesystem::path value = [] {
        const char* env = std::getenv("NINFER_TRUECOLD_T1_DIR");
        return env && *env ? std::filesystem::path(env) : std::filesystem::path{};
    }();
    return value;
}
inline bool enabled(const ProgramImpl& p) {
    if (root().empty()) return false;
    if (p.max_concurrency != 1 || p.kv_storage != KvCacheStorage::Rk4V4 ||
        p.speculative_backend != SpeculativeBackend::DFlash2 || p.draft_window != 7)
        throw std::runtime_error("true-cold T1 diagnostic requires RK4V4 / DFlash2 K7 / C1");
    return true;
}
inline std::filesystem::path directory(const std::string& name) {
    std::filesystem::create_directories(root());
    auto path = root() / name;
    if (!std::filesystem::create_directory(path))
        throw std::runtime_error("true-cold diagnostic refuses to overwrite a capture");
    return path;
}
inline void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    if (!file.good()) throw std::runtime_error("true-cold diagnostic write failed");
}
inline std::string hash(std::span<const std::byte> bytes) {
    return crypto::sha256_hex(crypto::sha256(bytes));
}
inline void write_json(const std::filesystem::path& path, const nlohmann::json& value) {
    const auto text = value.dump(2);
    write_bytes(path, std::as_bytes(std::span(text.data(), text.size())));
}
inline void snapshot(ProgramImpl& p, const SequenceState& sequence, const char* phase) {
    if (!enabled(p)) return;
    const auto frontier = sequence.continuation.metadata.text_kv_valid;
    if (frontier != 3066 && frontier != 3093)
        throw std::runtime_error("unexpected true-cold snapshot frontier");
    const auto path = directory(std::string(phase) + "-" + std::to_string(frontier));
    p.device.synchronize();
    CUDA_CHECK(cudaStreamSynchronize(p.device.transfer_stream()));
    auto& history = *p.text_kv_histories;
    auto& pool = history.pages().physical_pool();
    const auto layout = plan_host_kv_page_layout(pool.geometry());
    if (pool.geometry().device_plane_order != PagedKVPlaneOrder::PageMajor)
        throw std::runtime_error("true-cold diagnostic expects native PageMajor KV");
    const unsigned count = (frontier + 63U) / 64U;
    std::vector<std::byte> kv(count * layout.page_stride); // Zero holes/uncommitted rows.
    for (unsigned page = 0; page < count; ++page) {
        const unsigned valid = std::min(64U, frontier - page * 64U);
        const auto handle = history.physical_page(sequence.kv->text.handle(), page);
        for (std::size_t plane = 0; plane < pool.plane_count(); ++plane) {
            const auto source = pool.page_plane(handle, plane);
            const auto& packed = layout.planes[plane];
            const auto& shape = pool.geometry().planes[plane];
            if (shape.tokens_per_row != 1) throw std::runtime_error("unexpected KV token span");
            CUDA_CHECK(cudaMemcpy2DAsync(kv.data() + page * layout.page_stride + packed.offset,
                packed.head_payload_bytes, source.data, source.nb[2],
                valid * shape.leading_extent * dtype_size(shape.dtype), shape.head_extent,
                cudaMemcpyDeviceToHost, p.device.stream()));
        }
    }
    const auto& storage = p.state_images->storage();
    const auto& schema = storage.layout();
    const auto source = storage.image(p.state_store->physical_slot(sequence.continuation.image));
    std::vector<std::byte> state(schema.image_stride);
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& field : schema.fields) {
        const auto offset = field.tensor.region.offset;
        CUDA_CHECK(cudaMemcpy2DAsync(state.data() + offset, field.layer_stride,
            static_cast<const std::byte*>(source.data) + offset, field.layer_stride,
            field.tensor.region.bytes, field.layers, cudaMemcpyDeviceToHost, p.device.stream()));
        fields.push_back({{"offset", offset}, {"bytes", field.tensor.region.bytes},
                          {"layers", field.layers}, {"layer_stride", field.layer_stride}});
    }
    CUDA_CHECK(cudaStreamSynchronize(p.device.stream()));
    write_bytes(path / "kv.bin", kv);
    write_bytes(path / "state.bin", state);
    write_json(path / "metadata.json", {{"frontier", frontier}, {"pages", count},
        {"page_stride", layout.page_stride}, {"kv_sha256", hash(kv)},
        {"state_sha256", hash(state)}, {"state_fields", fields},
        {"ledger_tokens", sequence.ledger.size()},
        {"ledger_sha256", hash(std::as_bytes(std::span(sequence.ledger)))},
        {"graph", p.use_cuda_graph}, {"prefill_chunk", p.prefill_chunk},
        {"scope", "read-only diagnostic; synchronization changes timing, not arithmetic"}});
}
inline void logits(ProgramImpl& p, const SequenceState& sequence, std::size_t row,
                   unsigned licensed, std::uint32_t base) {
    if (!enabled(p)) return;
    // Column zero at base=3093 predicts generated token 2; token 1 came from prefill.
    constexpr std::uint32_t decision = 3093 + 30;
    if (base > decision || decision - base >= licensed) return;
    const auto column = decision - base;
    const auto& tensor = p.io.dflash_decode->target_logits;
    const auto vocab = p.parameters.model.resources().public_token_count;
    if (vocab < 2 || vocab > static_cast<std::size_t>(tensor.ne[0]))
        throw std::runtime_error("invalid diagnostic vocabulary");
    const auto path = directory("generated-step-32");
    const auto bytes = static_cast<std::size_t>(vocab) * dtype_size(tensor.dtype);
    std::vector<std::byte> raw(bytes);
    const auto* source = static_cast<const std::byte*>(tensor.data) +
        row * tensor.nb[2] + column * tensor.nb[1];
    CUDA_CHECK(cudaMemcpyAsync(raw.data(), source, raw.size(), cudaMemcpyDeviceToHost, p.device.stream()));
    CUDA_CHECK(cudaStreamSynchronize(p.device.stream()));
    std::vector<float> values(vocab);
    for (std::size_t token = 0; token < values.size(); ++token) {
        if (tensor.dtype == DType::FP32) std::memcpy(&values[token], raw.data() + token * 4, 4);
        else if (tensor.dtype == DType::BF16) {
            std::uint16_t bits;
            std::memcpy(&bits, raw.data() + token * 2, 2);
            values[token] = std::bit_cast<float>(std::uint32_t(bits) << 16);
        } else throw std::runtime_error("unexpected diagnostic logit dtype");
        if (!std::isfinite(values[token])) throw std::runtime_error("nonfinite diagnostic logit");
    }
    std::vector<std::size_t> ids(values.size());
    std::iota(ids.begin(), ids.end(), 0);
    const auto count = std::min<std::size_t>(20, ids.size());
    std::partial_sort(ids.begin(), ids.begin() + count, ids.end(), [&](auto a, auto b) {
        return values[a] == values[b] ? a < b : values[a] > values[b];
    });
    nlohmann::json top = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) top.push_back({{"token", ids[i]}, {"logit", values[ids[i]]}});
    write_bytes(path / "logits.bin", raw);
    write_json(path / "metadata.json", {{"step", 32}, {"base_execution_frontier", base},
        {"column", column}, {"licensed", licensed}, {"dtype", tensor.dtype == DType::FP32 ? "fp32" : "bf16"},
        {"vocabulary", vocab}, {"top20", top}, {"top2_margin", values[ids[0]] - values[ids[1]]},
        {"ledger_tokens", sequence.ledger.size()},
        {"ledger_sha256", hash(std::as_bytes(std::span(sequence.ledger)))},
        {"logits_sha256", hash(raw)},
        {"licensed_token", p.dflash_host_egress->licensed_tokens[row * (p.draft_window + 1) + column]}});
}
} // namespace ninfer::models::qwen3_5::detail::truecold_diagnostic
