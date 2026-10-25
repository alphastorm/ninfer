#include "models/qwen4_exp/load.h"
#include "models/qwen4_exp/program/resources.h"
#include "artifact/fixture.h"
#include "core/transfer.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <sys/resource.h>

namespace {
using namespace ninfer;
namespace q4 = models::qwen4_exp;
using test::artifact_fixture::require;
constexpr std::size_t GiB = 1024ULL * 1024 * 1024;

void placement(const q4::LoadPlan& load, Device device) {
    const auto plan = load.placement(device);
    require(plan.registered_host_objects.size() == 96,
            "Main banks must have one Host backing per parent");
    require(plan.registered_host_bytes >= 67947921408ULL &&
                plan.registered_host_bytes < 67947921408ULL + 2 * 1024 * 1024,
            "Main Host bank capacity differs");
    require(plan.device_capacity_bytes < 11 * GiB, "PLE or Main banks were placed on GPU");
    require(load.ple_table().parameters().rows == 320001536, "PLE table extent");
}

void payloads(const artifact::Reader& reader, const q4::Model& model) {
    const auto& w = model.weights();
    const auto& g = std::get<q4::GdnWeights>(w.text.layers[0].mixer);
    const auto& q = std::get<q4::QsaWeights>(w.text.layers[3].mixer);
    for (const auto id :
         {w.text.embedding, w.text.head, g.convolution, g.query, q.key, q.gate,
          w.text.layers.back().moe.gate, w.mtp->layer.moe.down, w.vision->layers[0].fc1}) {
        const auto& weight = model.weight(id);
        const auto& parent = *weight.view.parts.front().parent;
        const auto& object = reader.directory().tensor(
            reader.directory().object_index.at(weight.source_objects.front()));
        for (const auto offset : {std::uint64_t{0}, parent.geometry.bytes - 64}) {
            std::array<std::byte, 64> expected{}, actual{};
            reader.read_into(object.offset + offset, expected);
            if (parent.device == Device::cpu())
                std::memcpy(actual.data(), parent.data + offset, 64);
            else
                CUDA_CHECK(
                    cudaMemcpy(actual.data(), parent.data + offset, 64, cudaMemcpyDeviceToHost));
            require(expected == actual, "materialized words differ from the artifact");
        }
    }
}

void check_parameters(const q4::Model& model, const q4::execution::Parameters& p) {
    require(model.expert_banks().size() == 49, "Main/MTP bank count");
    for (std::size_t l = 0; l < 48; ++l) {
        const auto& source = model.expert_banks()[l];
        require(source.gate_up.parent->device == Device::cpu() &&
                    source.gate_up_upload.device_address,
                "Main source must remain registered Host memory");
        require(!p.text.layers[l].moe.resident.count, "Main Parameters acquired mutable GPU cache");
        for (std::uint32_t e : {0U, 17U, 511U}) {
            const auto gu   = matrix_weight(source.gate_up, e);
            const auto down = matrix_weight(source.down, e);
            require(gu.n == 1280 && gu.k == 2560 && down.n == 2560 && down.k == 640,
                    "expert native geometry");
            require(gu.weight_scale_divisor == source.gate_up.divisor(e) &&
                        down.weight_scale_divisor == source.down.divisor(e),
                    "expert divisor addressing");
        }
    }
    require(p.mtp && p.vision && p.mtp->embedding.qdata == p.text.embedding.qdata &&
                p.mtp->head.weight.qdata == p.text.head.weight.qdata,
            "MTP shared matrices");
    require(p.mtp->layer.moe.resident.count == 512 &&
                p.mtp->layer.moe.profile.routed.ffn.gate_up_policy == ops::LinearPolicy::AllowA8,
            "MTP resident FP8 bank");
    const auto& gdn = std::get<q4::execution::GdnParameters>(p.text.layers[0].mixer);
    require(gdn.convolution.ne[0] == 10240 && gdn.convolution.ne[1] == 4 &&
                p.text.ple.convolution.ne[0] == 10240,
            "stored convolution layout");
}

void ple_window(q4::Resources& resources, const q4::Model& model, DeviceContext& device) {
    const std::array<std::int32_t, 3> ids{123, 456, 789};
    const auto eos = model.config().text.ple.eos;
    const q4::ple::InputRow row{{1, 1, 0, {eos, eos}}, ids};
    const auto ticket = resources.ple_prepare->prepare(3, {&row, 1});
    require(ticket.has_value(), "PLE slot unavailable");
    const std::array<std::uint32_t, 1> valid{3};
    resources.ple_prepare->seal(*ticket, valid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto status = resources.ple_prepare->poll(*ticket);
        if (status.error) std::rethrow_exception(status.error);
        if (status.status == q4::ple::disk::PrepareStatus::HostReady && status.drained) break;
        require(std::chrono::steady_clock::now() < deadline, "PLE timed out");
        std::this_thread::yield();
    }
    const auto host = resources.ple_prepare->embedding(*ticket);
    resources.ple_upload->upload(host, resources.ple_transfer());
    auto e = resources.ple_upload->embedding(*ticket, device.execution_view());
    DeviceBuffer copy(device.device(), e.bytes());
    copy_bytes_async(copy.span(), {e.data, e.bytes(), device.device()}, device.execution_view());
    resources.ple_upload->record_consumed(*ticket, device.execution_view());
    device.synchronize();
    std::vector<std::byte> bytes(e.bytes());
    copy.copy_to_host(bytes.data(), bytes.size());
    require(std::memcmp(bytes.data(), host.values.data, bytes.size()) == 0,
            "PLE upload changed words");
    require(resources.ple_upload->try_release(*ticket), "PLE consumer did not retire");
    resources.ple_prepare->release(*ticket);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 77;
    try {
        DeviceContext device;
        const bool materialize = argc > 2 && std::string_view(argv[2]) == "--materialize";
        std::unique_ptr<q4::Model> model;
        std::shared_ptr<const q4::execution::ModelProfile> profile;
        std::optional<q4::ExecutionPlan> selected;
        {
            artifact::Reader reader(argv[1]);
            {
                auto scoring           = EngineOptions{};
                scoring.purpose        = EnginePurpose::CausalScoring;
                scoring.use_cuda_graph = false;
                scoring.enable_vision  = false;
                scoring.max_context    = 4096;
                scoring.prefill_chunk  = 4096;
                scoring.kv_capacity    = KvCapacityPolicy::explicit_capacity(4096);
                scoring.context_cache  = {
                     .enabled = false, .device_state_slots = 0, .host_capacity_bytes = 0};
                auto load       = q4::plan_load(reader, models::load_options(scoring));
                auto p          = q4::execution::describe_model(load);
                const auto plan = q4::plan_execution(p, load.ple_table(), scoring, device, 8 * GiB);
                require(plan && !plan->persistent.buffers.logits &&
                            !plan->persistent.buffers.token_counts &&
                            plan->pinned_output_bytes == 4096 &&
                            plan->workspace.finish >= 248320ULL * 1024 * 2 + 8192,
                        "scoring resource lifetime");
            }
            for (const bool vision : {false, true})
                for (const bool mtp : {false, true}) {
                    EngineOptions options;
                    options.use_cuda_graph  = false;
                    options.enable_vision   = vision;
                    options.max_context     = 4096;
                    options.prefill_chunk   = 4096;
                    options.max_concurrency = 1;
                    options.kv_capacity     = KvCapacityPolicy::explicit_capacity(4096);
                    options.context_cache.host_capacity_bytes = 256 * 1024 * 1024;
                    if (mtp) {
                        options.speculative.backend      = SpeculativeBackend::Mtp;
                        options.speculative.draft_tokens = 5;
                    }
                    auto load = q4::plan_load(reader, models::load_options(options));
                    profile   = q4::execution::describe_model(load);
                    placement(load, device.device());
                    const auto weights = load.placement(device.device()).device_capacity_bytes;
                    selected = q4::plan_execution(profile, load.ple_table(), options, device,
                                                  16 * GiB - weights - 256 * 1024 * 1024);
                    require(selected.has_value(), "16 GiB plan must fit this context");
                    require(selected->ple.max_positions == 4096,
                            "PLE Mmax differs from actual call capacity");
                    require(selected->profile->mtp.has_value() == mtp &&
                                selected->persistent.mtp.has_value() == mtp,
                            "MTP selection leaked resources");
                    if (vision) {
                        auto long_context        = options;
                        long_context.max_context = 65536;
                        const auto workspace     = q4::execution::plan_workspace(
                            *profile, long_context, 4096, device.multiprocessor_count(), nullptr);
                        require(workspace.vision->max_merged_tokens == 16384,
                                "Vision encoder reserved aggregate prompt capacity");
                    }
                    std::cout << "vision=" << vision << " mtp=" << mtp << " weights=" << weights
                              << " state_stride=" << selected->persistent.state.images.image_stride
                              << " persistent=" << selected->persistent.bytes
                              << " workspace=" << selected->workspace.bytes
                              << " cache=" << selected->moe->cache.config.capacity_bytes
                              << " windows=" << selected->moe->window_count
                              << " runtime=" << selected->device_bytes()
                              << " ple_host=" << selected->ple_prepare_bytes << '\n';
                    if (vision == mtp) {
                        auto multi            = options;
                        multi.max_concurrency = 8;
                        multi.kv_capacity     = KvCapacityPolicy::automatic();
                        for (const auto format :
                             {KvCacheStorage::BFloat16, KvCacheStorage::Int8Group64,
                              KvCacheStorage::Fp8E4M3Row256, KvCacheStorage::Nvfp4Group16,
                              KvCacheStorage::Fp8KeyNvfp4Value}) {
                            multi.kv_cache = format;
                            auto plan = q4::plan_execution(profile, load.ple_table(), multi, device,
                                                           16 * GiB - weights - 256 * 1024 * 1024);
                            require(plan && plan->capacity.resolved_tokens == 4096 &&
                                        plan->ple.max_positions == 4096,
                                    "mixed-batch automatic capacity");
                            require(plan->device_bytes() <= 16 * GiB - weights - 256 * 1024 * 1024,
                                    "execution exceeds Device budget");
                        }
                        require(!q4::plan_execution(profile, load.ple_table(), options, device, 1),
                                "insufficient Device budget admitted");
                    }
                    if (vision && mtp && materialize) {
                        model = q4::materialize_model(
                            q4::resolve_load(std::move(load), device.device()), device);
                        payloads(reader, *model);
                    }
                }
        }
        if (materialize) {
            const q4::execution::Parameters parameters(*model, profile);
            check_parameters(*model, parameters);
            q4::Resources resources(parameters, std::move(*selected), device);
            ple_window(resources, *model, device);

            struct rusage usage {};

            getrusage(RUSAGE_SELF, &usage);
            const auto& s = model->storage_stats();
            std::cout << "loaded host=" << s.host_capacity_bytes
                      << " registered=" << s.registered_host_bytes
                      << " gpu=" << s.device_capacity_bytes << " staging=" << s.peak_staging_bytes
                      << " read=" << s.read_bytes << " rss_kib=" << usage.ru_maxrss << '\n';
        }
        std::cout << "PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
