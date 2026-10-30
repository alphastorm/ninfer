#include "ninfer/ops/moe.h"
#include "artifact/binder.h"
#include "artifact/fixture.h"
#include "artifact/reader.h"
#include "core/decode_graph.h"
#include "models/qwen3_5/load.h"
#include "ninfer/ops/embedding.h"
#include "ninfer/ops/linear_split.h"
#include "ops/quantized_weight.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {

using namespace ninfer;
using namespace ninfer::models;
namespace q35 = ninfer::models::qwen3_5;
using ninfer::test::artifact_fixture::require;

struct Sample {
    std::uint64_t offset;
    std::vector<std::byte> bytes;
};

void check_mapped_embedding(const artifact::Reader& reader, const q35::Model& model,
                             DeviceContext& device) {
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    if (!model.options().dflash2()) return;
    const auto& bound = model.weight(model.weights().text.token_embedding);
    const auto mapped = native_weight(bound.view);
    const auto region = contiguous_weight_region(bound.view);
    const auto& parent = *region.parent;
    cudaPointerAttributes attributes{};
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, parent.data));
    require(attributes.type == cudaMemoryTypeHost && attributes.devicePointer == parent.data,
            "native DFlash2 embedding is not CUDA-mapped pinned Host storage");
    require(bound.source_objects.size() == 1,
            "mapped embedding proof requires its native contiguous parent");
    const auto object = reader.find(bound.source_objects.front());
    const auto offset = reader.directory().tensor(object).offset;
    DeviceBuffer resident(device.device(), parent.geometry.bytes);
    std::vector<std::byte> source(1024 * 1024), received(source.size());
    const auto compare_payload = [&](bool upload) {
        for (std::size_t begin = 0; begin < parent.geometry.bytes; begin += source.size()) {
            const auto count = std::min(source.size(), parent.geometry.bytes - begin);
            reader.read_into(offset + begin, std::span(source).first(count));
            CUDA_CHECK(cudaMemcpy(received.data(), parent.data + begin, count,
                                  cudaMemcpyDeviceToHost));
            require(std::equal(source.begin(), source.begin() + count, received.begin()),
                    "mapped embedding payload is not byte-identical to the artifact");
            if (upload) resident.copy_from_host(source.data(), count, begin);
        }
    };
    compare_payload(true);
    const WeightParent reference_parent{parent.geometry, device.device(),
                                        static_cast<const std::byte*>(resident.data()),
                                        parent.weight_divisors};
    const WeightView reference_view{bound.view.shape,
                                    {{&reference_parent, region.begin, region.end}}};
    const auto reference = native_weight(reference_view);
    for (const auto width : {1, 4, 8, 1024}) {
        std::vector<std::int32_t> ids(width);
        for (int i = 0; i < width; ++i) {
            ids[i] = i == 0 ? mapped.n - 1 : i == 1 ? 0
                : static_cast<std::int32_t>((std::uint64_t(i) * 2654435761U) % mapped.n);
        }
        DeviceBuffer input(device.device(), ids.size() * sizeof(ids[0]));
        input.copy_from_host(ids.data(), input.size());
        DeviceBuffer actual(device.device(), std::size_t(mapped.k) * width * 2);
        DeviceBuffer expected(device.device(), actual.size());
        Tensor tokens(input.span(), DType::I32, {width});
        Tensor output(actual.span(), DType::BF16, {mapped.k, width});
        Tensor oracle(expected.span(), DType::BF16, {mapped.k, width});
        ops::embedding(tokens, reference, oracle, device.execution_view());
        ops::embedding(tokens, mapped, output, device.execution_view());
        device.synchronize();
        std::vector<std::byte> expected_bytes(expected.size()), actual_bytes(actual.size());
        expected.copy_to_host(expected_bytes.data(), expected_bytes.size());
        actual.copy_to_host(actual_bytes.data(), actual_bytes.size());
        require(actual_bytes == expected_bytes, "mapped embedding eager gather changed BF16 bytes");
        DecodeGraphDefinition definition;
        definition.capture(device.stream(), [&] {
            ops::embedding(tokens, mapped, output, device.execution_view());
        });
        DecodeGraphExecutable graph;
        graph.instantiate(definition);
        for (int replay = 0; replay < 2; ++replay) {
            actual.fill(0xa5);
            graph.launch(device.stream());
            device.synchronize();
            actual.copy_to_host(actual_bytes.data(), actual_bytes.size());
            require(actual_bytes == expected_bytes,
                    "mapped embedding graph replay changed BF16 gather bytes");
        }
    }
    compare_payload(false);
    std::cout << "mapped embedding byte identity and eager/graph widths1,4,8,1024 passed\n";
#else
    (void)reader;
    (void)model;
    (void)device;
#endif
}

void sample_converted(const artifact::Reader& reader, const q35::LoadPlan& plan,
                       std::map<std::string, std::vector<Sample>>& samples,
                       std::uint64_t& expected_h2d) {
    if (!plan.converted_device_bytes()) return;
    namespace oracle = ninfer::test::quantized_weight;
    std::set<std::size_t> parents;
    for (const auto& layer : plan.weights().draft->layers) {
        for (const auto id : {layer.mlp.gate, layer.mlp.up}) {
            for (const auto& part : plan.parameter(id).binding.parts) {
                const auto& source = reader.geometry(part.object);
                if (source.format != QType::Q8_G32_FP16 ||
                    !parents.insert(part.object.index).second) continue;
                const auto target = weight_geometry(QType::Q4_G64_FP16, QuantLayout::RowSplit,
                                                      source.shape);
                const auto& object = reader.directory().tensor(part.object);
                expected_h2d += target.code_bytes + target.scale_bytes;
                const auto single = weight_geometry(QType::Q8_G32_FP16, QuantLayout::RowSplit,
                    std::array<std::uint64_t, 2>{1, source.shape[1]});
                for (const auto row : {std::uint64_t(0), source.shape[0] / 2, source.shape[0] - 1}) {
                    std::vector<std::uint8_t> encoded(single.bytes);
                    auto bytes = std::as_writable_bytes(std::span(encoded));
                    reader.read_into(object.offset + row * source.code_bytes_per_row,
                                     bytes.first(single.code_bytes));
                    reader.read_into(object.offset + source.scale_offset +
                                         row * source.scale_row_bytes,
                                     bytes.subspan(single.scale_offset, single.scale_bytes));
                    const auto decoded = oracle::decode_row_split_lowbit(
                        encoded, 1, static_cast<int>(source.shape[1]),
                        static_cast<int>(source.padded_columns), QType::Q8_G32_FP16);
                    const auto expected = oracle::pack_q4_row_split(
                        decoded, 1, static_cast<int>(source.shape[1]));
                    const auto expected_bytes = std::as_bytes(std::span(expected.payload));
                    samples[object.id].push_back({
                        row * target.code_bytes_per_row,
                        {expected_bytes.begin(), expected_bytes.begin() + expected.code_plane_bytes}});
                    samples[object.id].push_back({
                        target.scale_offset + row * target.scale_row_bytes,
                        {expected_bytes.begin() + expected.scale_plane_offset, expected_bytes.end()}});
                }
            }
        }
    }
}

void check_selection(const q35::LoadPlan& plan, const LoadOptions& options) {
    const auto& weights = plan.weights();
    require(weights.vision.has_value() == options.vision,
            "Vision demand differs from startup selection");
    require(weights.mtp.has_value() == (options.speculative == SpeculativeBackend::Mtp),
            "MTP selection differs");
    require(weights.draft.has_value() == (options.speculative == SpeculativeBackend::DFlash ||
                                          options.speculative == SpeculativeBackend::DFlash2),
            "draft selection differs");
    require(weights.proposal.has_value() == options.proposal_enabled(),
            "proposal selection differs");
    require(weights.text.layers.size() == plan.config().text.num_hidden_layers,
            "layer data differs from instance config");
    const auto check_head = [&](ninfer::models::WeightId head, ninfer::models::WeightUseId use,
                                const std::string& input) {
        require(use.parameter == head && plan.uses(head)[use.use_index].input == input,
                "shared output head selected the wrong mathematical Use");
    };
    check_head(weights.text.output_head, weights.text.output_head_use, "text/final_hidden");
    if (weights.mtp) {
        check_head(weights.mtp->output_head, weights.mtp->output_head_use, "mtp/final_hidden");
        require(weights.mtp->token_embedding == weights.text.token_embedding,
                "MTP embedding did not share target");
    }
    if (weights.draft) {
        check_head(weights.draft->output_head, weights.draft->output_head_use,
                   std::string(options.speculative_component()) + "/final_hidden");
        require(weights.draft->token_embedding == weights.text.token_embedding,
                "draft embedding did not share target");
    }
    if (!options.vision) {
        require(plan.resources().preprocessor_config_json.empty() &&
                    plan.resources().video_preprocessor_config_json.empty(),
                "Text-only loading retained Vision resources");
    }
}

void check_placement(const q35::ResolvedLoadPlan& plan) {
    const auto check = [](const auto& objects, std::uint64_t capacity,
                          std::uint64_t alignment = 1) {
        std::set<std::size_t> parents;
        std::uint64_t end = 0;
        for (const auto& item : objects) {
            require(parents.insert(item.object.index).second,
                    "parent was planned twice at one location");
            require(item.region.offset >= end && item.region.offset % item.region.alignment == 0,
                    "weight placement overlaps or is unaligned");
            end = item.region.offset + item.region.bytes;
        }
        require((end + alignment - 1) / alignment * alignment == capacity,
                "weight capacity differs from placements");
    };
    check(plan.materialization().device_objects, plan.materialization().device_capacity_bytes);
    const auto& placement = plan.materialization();
    check(placement.ordinary_host_objects,
          placement.host_capacity_bytes - placement.registered_host_bytes);
    check(placement.registered_host_objects, placement.registered_host_bytes,
          placement.host_alignment);
}

// Exercise native grouping after Reader destruction, independently of Program selection.
void check_native_inputs(const q35::Model& model) {
    const auto split = [](std::span<const ops::WeightInput> inputs, bool two_outputs) {
        const auto weights = ops::prepare_projection_weights(inputs);
        std::vector<std::int32_t> rows;
        for (const auto& input : inputs)
            rows.push_back(static_cast<std::int32_t>(input.weight.shape[0]));
        if (two_outputs) rows = {rows[0] + rows[1] + rows[2], rows[3]};
        return ops::linear_split_workspace_capacity_bytes(weights, rows, 1, 1);
    };
    const auto linear = [&](ninfer::models::WeightId id) {
        return ops::prepare_linear_weight(model.input(id));
    };
    const auto dense = [&](const q35::DenseWeights& weights) {
        (void)ops::prepare_linear_swiglu_weight(model.input(weights.gate), model.input(weights.up));
        (void)linear(weights.down);
    };
    const auto block = [&](const q35::BlockWeights& weights, bool mtp = false) {
        if (const auto* attention = std::get_if<q35::AttentionWeights>(&weights.mixer)) {
            if (mtp) {
                const std::array bank{model.input(attention->query), model.input(attention->key),
                                      model.input(attention->gate), model.input(attention->value)};
                (void)ops::prepare_linear_weight(bank);
                for (const auto& input : bank) { (void)ops::prepare_linear_weight(input); }
            } else {
                (void)split(std::array<ops::WeightInput, 4>{{model.input(attention->query),
                                                             model.input(attention->key),
                                                             model.input(attention->gate),
                                                             model.input(attention->value)}},
                            false);
            }
            (void)linear(attention->output);
        } else {
            const auto& gdn = std::get<q35::GdnWeights>(weights.mixer);
            (void)split(
                std::array<ops::WeightInput, 4>{{model.input(gdn.query), model.input(gdn.key),
                                                 model.input(gdn.value), model.input(gdn.z)}},
                true);
            (void)ops::prepare_gdn_control_proj_weights(model.input(gdn.a_projection),
                                                        model.input(gdn.b_projection));
            (void)linear(gdn.output);
        }
        if (const auto* mlp = std::get_if<q35::DenseWeights>(&weights.ffn)) {
            dense(*mlp);
        } else {
            const auto& moe  = std::get<q35::MoeWeights>(weights.ffn);
            const auto& bank = model.expert_banks()[moe.bank.index];
            for (const auto e : {0U, static_cast<unsigned>(moe.experts.size() - 1)}) {
                const auto gu       = matrix_weight(bank.gate_up, e);
                const auto down     = matrix_weight(bank.down, e);
                const auto gate_ref = native_weight(model.weight(moe.experts[e].gate).view);
                const auto down_ref = native_weight(model.weight(moe.experts[e].down).view);
                require(gu.qdata == gate_ref.qdata && down.qdata == down_ref.qdata &&
                            down.qhigh == down_ref.qhigh && down.scales == down_ref.scales,
                        "bank source differs from logical expert Binding");
            }
            if (bank.gate_up.parent->device == Device::cpu()) {
                require(bank.gate_up_upload.host.data == bank.gate_up.parent->data &&
                            bank.down_upload.host.data == bank.down.parent->data &&
                            bank.gate_up_upload.device_address && bank.down_upload.device_address,
                        "Host expert bank has no matching registered source");
                return;
            }
            const auto device = device_expert_bank(bank.gate_up, bank.down);
            require(device.device == bank.gate_up.parent->device &&
                        device.count == moe.experts.size(),
                    "native expert view differs from bank");
            const std::array control{model.input(moe.router), model.input(moe.shared_score)};
            (void)ops::prepare_linear_weight(control);
            const ops::SwiGLUWeights shared{
                ops::prepare_linear_swiglu_weight(model.input(moe.shared.gate),
                                                  model.input(moe.shared.up)),
                ops::prepare_linear_weight(model.input(moe.shared.down))};
            const ops::MoeProfile profile{
                ops::routed_swiglu_profile(
                    bank.gate_up, bank.down,
                    std::get<q35::MoeConfig>(model.config().text.ffn).num_experts_per_tok,
                    model.input(moe.experts.front().gate).policy,
                    model.input(moe.experts.front().down).policy),
                ops::swiglu_profile(shared)};
            (void)ops::topk_softmax_swiglu_moe_workspace_capacity_bytes(
                profile, ops::MoeEpilogue::AddResidual, 1, 1);
        }
    };
    const auto& weights = model.weights();
    (void)native_weight(model.weight(weights.text.token_embedding).view);
    (void)ops::prepare_linear_weight(model.input(weights.text.output_head_use));
    for (const auto& layer : weights.text.layers) { block(layer); }
    if (weights.vision) {
        const auto& vision = *weights.vision;
        (void)linear(vision.patch_embedding);
        for (const auto& layer : vision.layers) {
            const std::array qkv{model.input(layer.query), model.input(layer.key),
                                 model.input(layer.value)};
            (void)ops::prepare_linear_weight(qkv);
            (void)linear(layer.output);
            (void)linear(layer.fc1);
            (void)linear(layer.fc2);
        }
        (void)linear(vision.merger_fc1);
        (void)linear(vision.merger_fc2);
    }
    if (weights.mtp) {
        (void)linear(weights.mtp->input_projection);
        block(weights.mtp->layer, true);
        (void)ops::prepare_linear_weight(model.input(weights.mtp->output_head_use));
    }
    if (weights.draft) {
        const auto& draft = *weights.draft;
        (void)linear(draft.feature_projection);
        for (const auto& layer : draft.layers) {
            const auto& a = layer.attention;
            (void)split(std::array<ops::WeightInput, 3>{{model.input(a.query), model.input(a.key),
                                                         model.input(a.value)}},
                        false);
            const std::array kv{model.input(a.context_key), model.input(a.context_value)};
            (void)ops::prepare_linear_weight(kv);
            (void)linear(a.output);
            dense(layer.mlp);
            if (layer.attention_conv) { (void)linear(layer.attention_conv->kernel_projection); }
            if (layer.mlp_conv) { (void)linear(layer.mlp_conv->kernel_projection); }
        }
        if (draft.selector) { (void)linear(draft.selector->hidden_projection); }
        (void)ops::prepare_linear_weight(model.input(draft.output_head_use));
    }
    if (model.weight(weights.text.output_head).uses.size() > 1) {
        ninfer::test::artifact_fixture::rejects<std::invalid_argument>(
            [&] { (void)model.input(weights.text.output_head); },
            "ambiguous shared output-head Use was silently selected");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path path;
        LoadOptions options;
        bool host_only = false, host_experts = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            const auto value = [&]() -> std::string {
                if (++i >= argc) { throw std::invalid_argument("missing value for " + arg); }
                return argv[i];
            };
            if (arg == "--artifact") {
                path = value();
            } else if (arg == "--vision") {
                options.vision = true;
            } else if (arg == "--host-only") {
                host_only = true;
            } else if (arg == "--host-experts") {
                host_experts = true;
            } else if (arg == "--scoring") {
                options.purpose = EnginePurpose::CausalScoring;
            } else if (arg == "--speculative") {
                const auto backend = value();
                if (backend == "none") {
                    options.speculative = SpeculativeBackend::None;
                } else if (backend == "mtp") {
                    options.speculative = SpeculativeBackend::Mtp;
                } else if (backend == "dflash") {
                    options.speculative = SpeculativeBackend::DFlash;
                } else if (backend == "dflash2") {
                    options.speculative = SpeculativeBackend::DFlash2;
                } else {
                    throw std::invalid_argument("unknown speculative backend");
                }
            } else if (arg == "--proposal") {
                const auto head = value();
                if (head == "full") {
                    options.proposal_head = ProposalHead::Full;
                } else if (head == "optimized") {
                    options.proposal_head = ProposalHead::Optimized;
                } else {
                    throw std::invalid_argument("unknown proposal head");
                }
            } else if (arg == "--help") {
                std::cout
                    << "--artifact PATH [--vision] [--speculative none|mtp|dflash|dflash2]\n"
                       "[--proposal full|optimized] [--host-only] [--host-experts] [--scoring]\n";
                return 0;
            } else {
                throw std::invalid_argument("unknown argument " + arg);
            }
        }
        if (path.empty()) {
            std::cout << "SKIP: supply an explicit --artifact\n";
            return 77;
        }
        std::unique_ptr<DeviceContext> device;
        std::unique_ptr<q35::Model> model;
        std::map<std::string, std::vector<Sample>> samples;
        std::uint64_t expected_h2d = 0;
        std::uint64_t expected_device_bytes = 0;
        {
            artifact::Reader reader(path);
            auto logical = q35::plan_load(reader, options);
            check_selection(logical, options);
            auto plan =
                q35::resolve_load(std::move(logical), Device::cuda(0),
                                  host_experts ? std::optional(Device::cpu()) : std::nullopt);
            check_placement(plan);
            if (host_only) {
                std::cout << path.filename().string() << ": Host binding passed, architecture="
                          << architecture_name(plan.logical().config().text.architecture)
                          << " V=" << plan.logical().resources().public_token_count
                          << " device_bytes=" << plan.logical().device_capacity_bytes(
                                 Device::cuda(0), host_experts ? Device::cpu() : Device::cuda(0))
                          << " converted_device_bytes=" << plan.logical().converted_device_bytes()
                          << " host_mapped_bytes=" << plan.logical().host_mapped_bytes()
                          << '\n';
                return 0;
            }
            const auto& placement = plan.materialization();
            expected_device_bytes = placement.device_capacity_bytes +
                                    plan.logical().converted_device_bytes();
            sample_converted(reader, plan.logical(), samples, expected_h2d);
            for (const auto& item : placement.device_objects) { expected_h2d += item.region.bytes; }
            auto parents = placement.device_objects;
            parents.insert(parents.end(), placement.ordinary_host_objects.begin(),
                           placement.ordinary_host_objects.end());
            parents.insert(parents.end(), placement.registered_host_objects.begin(),
                           placement.registered_host_objects.end());
            for (const auto& item : parents) {
                const auto& object   = reader.directory().tensor(item.object);
                const auto& geometry = reader.geometry(item.object);

                const auto size = std::min<std::uint64_t>(64, object.bytes);
                std::set<std::uint64_t> offsets{0, (object.bytes - size) / 2, object.bytes - size};
                if (geometry.high_bytes) { offsets.insert(geometry.high_offset); }
                if (geometry.scale_bytes) { offsets.insert(geometry.scale_offset); }
                for (const auto offset : offsets) {
                    const auto count = std::min<std::uint64_t>(size, object.bytes - offset);
                    samples[object.id].push_back(
                        {offset, reader.read_range(object.offset + offset, count)});
                }
            }
            device = std::make_unique<DeviceContext>();
            model  = q35::materialize_model(std::move(plan), *device);
            check_mapped_embedding(reader, *model, *device);
        }
        require(model->storage_stats().device_capacity_bytes == expected_device_bytes,
                "effective weight Device accounting differs from startup planning");
        require(model->storage_stats().h2d_bytes == expected_h2d,
                "H2D did not cover all selected parent bytes");
        std::set<const WeightParent*> visited;
        std::set<std::string> visited_objects;
        std::size_t draft_weights = 0;
        for (const auto& weight : model->weight_data()) {
            const bool draft_weight =
                weight.name.starts_with("dflash/") || weight.name.starts_with("dflash2/");
            if (options.speculative != SpeculativeBackend::DFlash &&
                options.speculative != SpeculativeBackend::DFlash2) {
                require(!draft_weight, "unselected draft weight was materialized");
            }
            draft_weights += draft_weight;
            require(weight.view.parts.size() == weight.source_objects.size(),
                    "diagnostic parent association lost");
            for (std::size_t i = 0; i < weight.view.parts.size(); ++i) {
                const auto* parent = weight.view.parts[i].parent;
                if (!visited.insert(parent).second) { continue; }
                visited_objects.insert(weight.source_objects[i]);
                for (const auto& sample : samples.at(weight.source_objects[i])) {
                    std::vector<std::byte> received(sample.bytes.size());
                    if (parent->device == Device::cpu()) {
                        std::memcpy(received.data(), parent->data + sample.offset, received.size());
                    } else {
                        CUDA_CHECK(cudaMemcpy(received.data(), parent->data + sample.offset,
                                              received.size(), cudaMemcpyDeviceToHost));
                    }
                    require(received == sample.bytes,
                            "resident parent differs from stored bytes or effective recipe oracle");
                }
            }
        }
        require(visited_objects.size() == samples.size(),
                "some resident parents are not reachable from model weights");
        check_native_inputs(*model);
        const auto ids = model->resources().tokenizer->encode("Hello");
        require(model->resources().tokenizer->decode(ids) == "Hello",
                "tokenizer did not survive Reader destruction");
        require(!model->resources().chat_template_jinja.empty(),
                "template bytes were lost after loading");
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS usage{};
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &usage, sizeof(usage))) {
            throw std::runtime_error("cannot query peak process working set");
        }
        const auto peak_rss_kib = usage.PeakWorkingSetSize / 1024;
#else
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const auto peak_rss_kib = usage.ru_maxrss;
#endif
        std::cout << path.filename().string()
                  << ": loading and native inputs passed, parents=" << visited.size()
                  << " device_bytes=" << model->storage_stats().device_capacity_bytes
                  << " host_bytes=" << model->storage_stats().host_capacity_bytes
                  << " h2d_bytes=" << model->storage_stats().h2d_bytes
                  << " materialization_seconds=" << model->storage_stats().upload_seconds
                  << " peak_rss_kib=" << peak_rss_kib
                  << " mtp_selected=" << model->weights().mtp.has_value()
                  << " draft_selected=" << model->weights().draft.has_value()
                  << " draft_weights=" << draft_weights
                  << " V=" << model->resources().public_token_count << '\n';
        model.reset();
        device->synchronize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
