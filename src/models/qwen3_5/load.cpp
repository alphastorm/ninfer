#include "models/qwen3_5/load.h"

#include "artifact/reader.h"
#include "models/qwen3_5/load/bindings.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ninfer::models::qwen3_5 {

struct LoadPlan::Impl {
    Config config;
    LoadOptions options;
    ModelWeights weights;
    std::vector<PendingWeight> pending;
    const artifact::Reader* reader = nullptr;
    artifact::PreparedResources prepared_resources;
    std::vector<moe::BankDescription> expert_banks;
    qwen::FrontendResources resources;
    InstanceInfo info;
    std::vector<WeightParent> geometry_parents;
    std::vector<WeightView> geometry_views;
    bool mapped_embedding = false;
    std::vector<LayoutRegion> converted_regions;
    std::size_t converted_bytes = 0;
    std::size_t mapped_bytes = 0;

    bool converted(artifact::ObjectHandle object) const {
        return !converted_regions.empty() && converted_regions[object.index].bytes != 0;
    }
};

LoadPlan::LoadPlan(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

LoadPlan::~LoadPlan()                              = default;
LoadPlan::LoadPlan(LoadPlan&&) noexcept            = default;
LoadPlan& LoadPlan::operator=(LoadPlan&&) noexcept = default;

const Config& LoadPlan::config() const { return impl_->config; }

const LoadOptions& LoadPlan::options() const { return impl_->options; }

ops::WeightInput LoadPlan::input(WeightUseId id) const {
    const auto& use = impl_->pending.at(id.parameter.index).uses.at(id.use_index);
    return ops::effective_weight_input(
        {impl_->geometry_views.at(id.parameter.index), use.policy, use.activation_input_divisor},
        impl_->options.native_prefill_policy);
}

ops::WeightInput LoadPlan::input(WeightId id) const {
    if (impl_->pending.at(id.index).uses.size() != 1) {
        throw artifact::ArtifactError("weight description requires an explicit Use");
    }
    return input(WeightUseId{id, 0});
}

const ModelWeights& LoadPlan::weights() const { return impl_->weights; }

const qwen::FrontendResources& LoadPlan::resources() const { return impl_->resources; }

std::span<const moe::BankDescription> LoadPlan::expert_banks() const { return impl_->expert_banks; }

const artifact::ParameterReference& LoadPlan::parameter(WeightId id) const {
    return impl_->pending.at(id.index).reference;
}

std::span<const WeightUse> LoadPlan::uses(WeightId id) const {
    return impl_->pending.at(id.index).uses;
}

LoadPlan plan_load(const artifact::Reader& reader, LoadOptions options) {
    auto out     = std::make_unique<LoadPlan::Impl>();
    out->reader  = &reader;
    out->options = options;
    out->config  = parse_config(reader.directory(), options);
    artifact::Binder binder(reader);
    out->resources = loading::bind_resources(binder, out->config);
    Bindings bindings(binder);
    const auto& text  = out->config.text;
    out->weights.text = loading::bind_text(bindings, text, options);
    if (out->config.vision) {
        out->weights.vision = qwen::bind_vision(bindings, *out->config.vision, text.hidden_size);
    }
    if (out->config.mtp) {
        out->weights.mtp = loading::bind_mtp(bindings, text, out->weights.text);
    }
    if (out->config.draft) {
        out->weights.draft =
            loading::bind_draft(bindings, *out->config.draft, text, out->weights.text,
                                std::string(options.speculative_component()));
    }
    if (options.proposal_enabled()) {
        const auto& proposal = reader.directory().component("text").proposal;
        if (!proposal) {
            throw artifact::ArtifactError("selected proposal head is absent from artifact");
        }
        out->weights.proposal = loading::bind_proposal(bindings, *proposal, text, options,
                                                       out->resources.public_token_count);
        if (out->config.draft && out->config.draft->dflash2) {
            const auto domain =
                proposal->indexed ? proposal->rows : out->resources.public_token_count;
            if (out->config.draft->dflash2->selector_top_k > domain) {
                throw artifact::ArtifactError(
                    "proposal domain is smaller than DFlash2 selector_top_k");
            }
        }
        if (out->weights.mtp) { out->weights.mtp->output_head = out->weights.proposal->head; }
        if (out->weights.draft) { out->weights.draft->output_head = out->weights.proposal->head; }
    }
    out->weights.text.output_head_use =
        bindings.use(out->weights.text.output_head, "text/final_hidden");
    if (out->weights.mtp) {
        out->weights.mtp->output_head_use =
            bindings.use(out->weights.mtp->output_head, "mtp/final_hidden");
    }
    if (out->weights.draft) {
        out->weights.draft->output_head_use =
            bindings.use(out->weights.draft->output_head,
                         std::string(options.speculative_component()) + "/final_hidden");
    }
    out->pending            = std::move(bindings.weights);
    out->expert_banks       = describe_expert_weights(reader, out->weights, out->pending);
    out->prepared_resources = std::move(binder).finish();
    out->info.name          = reader.directory().metadata.value(
        "name", std::string(architecture_name(text.architecture)));
    out->geometry_parents.resize(reader.directory().objects.size());
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    if (options.dflash2()) {
        if (!out->weights.draft) {
            throw artifact::ArtifactError("DFlash2 requires the artifact's draft component");
        }
        out->converted_regions.resize(reader.directory().objects.size());
        out->mapped_embedding = true;
        std::vector<bool> gate_up(out->pending.size());
        for (const auto& layer : out->weights.draft->layers) {
            gate_up[layer.mlp.gate.index] = true;
            gate_up[layer.mlp.up.index] = true;
        }
        // Convert a shared physical parent once, without changing any unrelated Use.
        LayoutBuilder converted;
        for (std::size_t i = 0; i < out->pending.size(); ++i) {
            if (!gate_up[i]) continue;
            for (const auto& part : out->pending[i].reference.binding.parts) {
                const auto& source = reader.geometry(part.object);
                if (source.format != QType::Q8_G32_FP16) continue;
                if (source.layout != QuantLayout::RowSplit || source.shape.size() != 2 ||
                    part.begin % source.shape.back() || part.end % source.shape.back()) {
                    throw artifact::ArtifactError("draft MLP conversion requires Q8 matrix rows");
                }
                auto& region = out->converted_regions[part.object.index];
                if (region.bytes) continue;
                auto& effective = out->geometry_parents[part.object.index].geometry;
                effective = weight_geometry(QType::Q4_G64_FP16, QuantLayout::RowSplit, source.shape);
                region = converted.add(effective.bytes, effective.alignment, "draft Q4 parent");
            }
        }
        for (std::size_t i = 0; i < out->pending.size(); ++i) {
            if (gate_up[i]) continue;
            for (const auto& part : out->pending[i].reference.binding.parts) {
                if (out->converted_regions[part.object.index].bytes) {
                    throw artifact::ArtifactError(
                        "draft gate/up Q8 parent is shared with an unconverted parameter");
                }
            }
        }
        out->converted_bytes = converted.finish();
        for (const auto& part :
             out->pending[out->weights.text.token_embedding.index].reference.binding.parts) {
            auto& geometry = out->geometry_parents[part.object.index].geometry;
            if (geometry.elements) continue; // Count each mapped physical parent once.
            geometry = reader.geometry(part.object);
            out->mapped_bytes = artifact::checked_add(out->mapped_bytes, geometry.bytes,
                                                       "host-mapped embedding capacity");
        }
    }
#endif
    for (const auto& pending : out->pending) {
        WeightView view{pending.reference.shape, {}};
        for (const auto& part : pending.reference.binding.parts) {
            auto& parent = out->geometry_parents.at(part.object.index);
            if (!parent.geometry.elements) parent.geometry = reader.geometry(part.object);
            view.parts.push_back({&parent, part.begin, part.end});
        }
        out->geometry_views.push_back(std::move(view));
    }
    return LoadPlan(std::move(out));
}

ResolvedLoadPlan::ResolvedLoadPlan(LoadPlan&& logical,
                                   artifact::MaterializationPlan&& materialization,
                                   std::vector<Device>&& devices,
                                   std::vector<Device>&& bank_devices)
    : materialization_(std::move(materialization)), logical_(std::move(logical)),
      devices_(std::move(devices)), bank_devices_(std::move(bank_devices)) {}

namespace {
struct Placement {
    artifact::MaterializationPlan storage;
    std::vector<Device> weights, banks;
};

template <class Data>
Placement describe_placement(const Data& data, Device cuda, Device expert_device) {
    if (cuda.type() != DeviceType::CUDA ||
        (expert_device != cuda && expert_device != Device::cpu())) {
        throw artifact::ArtifactError("weight placement requires the selected GPU or CPU");
    }
    Placement out;
    out.weights.assign(data.pending.size(), cuda);
    out.banks.assign(data.expert_banks.size(), cuda);
    for (const auto& layer : data.weights.text.layers)
        if (const auto* parameters = std::get_if<MoeWeights>(&layer.ffn))
            out.banks[parameters->bank.index] = expert_device;
    const auto place = [&](const BlockWeights& layer) {
        if (const auto* moe = std::get_if<MoeWeights>(&layer.ffn)) {
            for (const auto& expert : moe->experts) {
                for (const auto id : {expert.gate, expert.up, expert.down}) {
                    out.weights[id.index] = out.banks[moe->bank.index];
                }
            }
        }
    };
    for (const auto& layer : data.weights.text.layers) place(layer);
    if (data.weights.mtp) place(data.weights.mtp->layer);
    if (data.mapped_embedding) {
        out.weights[data.weights.text.token_embedding.index] = Device::cpu();
    }
    std::vector<artifact::PlacementRequest> requests;
    for (std::size_t i = 0; i < data.pending.size(); ++i) {
        for (const auto& part : data.pending[i].reference.binding.parts) {
            if (data.converted(part.object)) continue;
            requests.push_back({part.object, out.weights[i],
                                out.weights[i] == Device::cpu() ? artifact::HostAccess::CudaTransfer
                                                                : artifact::HostAccess::CpuOnly});
        }
    }
    out.storage = artifact::plan_materialization(*data.reader, requests);
    return out;
}
} // namespace

artifact::MaterializationPlan LoadPlan::placement(Device cuda, Device main_experts) const {
    return describe_placement(*impl_, cuda, main_experts).storage;
}

std::size_t LoadPlan::device_capacity_bytes(Device cuda, Device main_experts) const {
    return artifact::checked_add(placement(cuda, main_experts).device_capacity_bytes,
                                  impl_->converted_bytes, "effective weight capacity");
}

std::size_t LoadPlan::converted_device_bytes() const { return impl_->converted_bytes; }

std::size_t LoadPlan::host_mapped_bytes() const { return impl_->mapped_bytes; }

ResolvedLoadPlan resolve_load(LoadPlan&& plan, Device cuda, std::optional<Device> main_experts) {
    if (!plan.impl_) throw artifact::ArtifactError("load plan was already consumed");
    auto placement = describe_placement(*plan.impl_, cuda, main_experts.value_or(cuda));
    placement.storage.resources = std::move(plan.impl_->prepared_resources);
    return ResolvedLoadPlan(std::move(plan), std::move(placement.storage),
                            std::move(placement.weights), std::move(placement.banks));
}

namespace {

template <class Data>
EffectiveWeightStorage materialize_effective(const Data& data,
                                             const artifact::MaterializedArtifact& backing,
                                             Device device) {
    EffectiveWeightStorage out;
    out.stats = backing.stats();
    if (!data.mapped_embedding && !data.converted_bytes) return out;
    out.parents.resize(data.geometry_parents.size());
    if (data.mapped_embedding) {
        for (const auto& part :
             data.pending[data.weights.text.token_embedding.index].reference.binding.parts) {
            const auto mapped = backing.host_transfer_view(part.object);
            if (!mapped.device_address || mapped.cuda_device != device) {
                throw artifact::ArtifactError("token embedding has no stable CUDA Host mapping");
            }
            auto& parent = out.parents[part.object.index];
            parent = backing.host_parent(part.object);
            parent.device = device;
            parent.data = static_cast<const std::byte*>(mapped.device_address);
        }
    }
    if (!data.converted_bytes) return out;
    const auto start = std::chrono::steady_clock::now();
    out.device = DeviceBuffer(device, data.converted_bytes);
    out.device.fill(0); // Includes inter-parent alignment gaps and RowSplit padding.
    constexpr std::uint64_t tile_rows = 64;
    std::vector<std::byte> codes, scales, q4_codes, q4_scales;
    for (std::size_t i = 0; i < data.converted_regions.size(); ++i) {
        const auto& region = data.converted_regions[i];
        if (!region.bytes) continue;
        const artifact::ObjectHandle object{i};
        const auto& source = data.reader->geometry(object);
        const auto& target = data.geometry_parents[i].geometry;
        const auto offset = data.reader->directory().tensor(object).offset;
        const auto destination = region.bind(out.device.span());
        out.parents[i] = {target, device, static_cast<const std::byte*>(destination.data), {}};
        for (std::uint64_t row = 0; row < source.shape[0]; row += tile_rows) {
            const auto rows = std::min(tile_rows, source.shape[0] - row);
            codes.resize(rows * source.code_bytes_per_row);
            scales.resize(rows * source.scale_row_bytes);
            q4_codes.resize(rows * target.code_bytes_per_row);
            q4_scales.resize(rows * target.scale_row_bytes);
            data.reader->read_into(offset + row * source.code_bytes_per_row, codes);
            data.reader->read_into(offset + source.scale_offset + row * source.scale_row_bytes,
                                   scales);
            loading::requantize_draft_mlp_rows(source.shape[1], rows, codes, scales,
                                               q4_codes, q4_scales);
            out.device.copy_from_host(q4_codes.data(), q4_codes.size(),
                                       region.offset + row * target.code_bytes_per_row);
            out.device.copy_from_host(q4_scales.data(), q4_scales.size(),
                                       region.offset + target.scale_offset +
                                           row * target.scale_row_bytes);
            out.stats.read_bytes += codes.size() + scales.size();
            out.stats.h2d_bytes += q4_codes.size() + q4_scales.size();
            out.stats.peak_staging_bytes = std::max(
                out.stats.peak_staging_bytes,
                std::uint64_t(codes.capacity() + scales.capacity() +
                              q4_codes.capacity() + q4_scales.capacity()));
        }
        ++out.stats.device_object_count;
    }
    out.stats.device_capacity_bytes += data.converted_bytes;
    out.stats.upload_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return out;
}

} // namespace

std::unique_ptr<Model> materialize_model(ResolvedLoadPlan&& plan, DeviceContext& device,
                                         const StartupObserver* observer) {
    if (!plan.logical_.impl_) { throw artifact::ArtifactError("load plan was already consumed"); }
    auto data    = std::move(plan.logical_.impl_);
    auto backing = artifact::materialize(*data->reader, std::move(plan.materialization_),
                                         device.transfer_view(), observer);
    auto effective = materialize_effective(*data, backing, device.device());
    std::vector<BoundWeight> bound;
    if (!data->mapped_embedding && !data->converted_bytes) {
        bound = resolve_weights(std::move(data->pending), backing, plan.devices_);
    } else {
        bound.reserve(data->pending.size());
        for (std::size_t i = 0; i < data->pending.size(); ++i) {
            auto& pending = data->pending[i];
            WeightView view{pending.reference.shape, {}};
            std::uint64_t covered = 0;
            for (const auto& part : pending.reference.binding.parts) {
                const bool replaced = data->converted(part.object) ||
                                      (data->mapped_embedding &&
                                       i == data->weights.text.token_embedding.index);
                const auto& parent = replaced ? effective.parents[part.object.index]
                                     : plan.devices_[i] == Device::cpu()
                                         ? backing.host_parent(part.object)
                                         : backing.device_parent(part.object);
                if (parent.device != (replaced ? device.device() : plan.devices_[i]) ||
                    part.begin >= part.end || part.end > parent.geometry.elements) {
                    throw artifact::ArtifactError(pending.reference.name +
                                                    ": invalid effective weight region");
                }
                covered = artifact::checked_add(covered, part.end - part.begin,
                                                  pending.reference.name);
                view.parts.push_back({&parent, part.begin, part.end});
            }
            if (covered != weight_element_count(view.shape)) {
                throw artifact::ArtifactError(pending.reference.name +
                                                ": effective coverage differs from logical shape");
            }
            bound.push_back({std::move(pending.reference.name), std::move(pending.source_objects),
                             std::move(view), std::move(pending.uses)});
        }
    }
    auto banks   = moe::bind_banks(data->expert_banks, backing, plan.bank_devices_);
    return std::unique_ptr<Model>(new Model(
        std::move(data->config), data->options, std::move(data->weights), std::move(bound),
        std::move(banks), std::move(data->resources), std::move(data->info), std::move(backing),
        std::move(effective)));
}

std::unique_ptr<Model> load_model(const std::filesystem::path& path, LoadOptions options,
                                  DeviceContext& device, const StartupObserver* observer) {
    artifact::Reader reader(path);
    return materialize_model(resolve_load(plan_load(reader, options), device.device()), device,
                             observer);
}

} // namespace ninfer::models::qwen3_5
