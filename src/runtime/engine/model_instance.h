#pragma once

#include "runtime/engine/kv_capacity.h"

#include <memory>
#include <utility>

namespace ninfer::runtime {

template <class Types>
struct ModelInstance {
    using ModelContract = Types;

    std::unique_ptr<typename Types::Model> model;
    const typename Types::Parameters parameters;
    typename Types::Frontend frontend;
    KvCapacityResolution kv_capacity_resolution;
    std::size_t device_budget_bytes = 0;
    std::optional<GraphMemorySummary> cuda_graph;
    std::int64_t startup_retained_bytes = 0;
    std::unique_ptr<typename Types::Program> program;

    ModelInstance(std::unique_ptr<typename Types::Model> source,
                  typename Types::Frontend prepared_frontend,
                  std::shared_ptr<const typename Types::Profile> profile)
        : model(std::move(source)), parameters(*model, std::move(profile)),
          frontend(std::move(prepared_frontend)) {}

    ModelInstance(const ModelInstance&)            = delete;
    ModelInstance& operator=(const ModelInstance&) = delete;
};

} // namespace ninfer::runtime
