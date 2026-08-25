#include "serve/server_identity.h"

#include "ninfer/build_info.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace ninfer::serve {
namespace {

nlohmann::json optional_identity(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

} // namespace

nlohmann::json server_identity_json(const ServeOptions& options, const ninfer::LoadSummary& load) {
    const ninfer::BuildInfo build = ninfer::build_info();
    return nlohmann::json{
        {"upstream_base_sha", build.upstream_base_sha},
        {"patch_stack_sha", build.patch_stack_sha},
        {"source_dirty", build.source_dirty},
        {"build_profile", build.build_profile},
        {"build_type", build.build_type},
        {"cxx_compiler", build.cxx_compiler},
        {"cuda_compiler", build.cuda_compiler},
        {"cuda_toolkit", build.cuda_toolkit},
        {"deployment_profile", optional_identity(options.deployment_profile)},
        {"binary_sha256", optional_identity(options.binary_sha256)},
        {"model_artifact_sha256", optional_identity(options.artifact_sha256)},
        {"config_sha256", optional_identity(options.config_sha256)},
        {"target", load.target},
        {"model_id", load.model_id},
        {"weights_id", load.weights_id},
    };
}

std::string format_server_identity(const ServeOptions& options, const ninfer::LoadSummary& load) {
    const ninfer::BuildInfo build = ninfer::build_info();
    const auto value_or_unknown = [](const std::string& value) -> const std::string& {
        static const std::string unknown = "unknown";
        return value.empty() ? unknown : value;
    };
    std::ostringstream out;
    out << "identity upstream=" << build.upstream_base_sha << " patch=" << build.patch_stack_sha
        << " dirty=" << (build.source_dirty ? "true" : "false")
        << " build=" << build.build_profile << " deployment="
        << value_or_unknown(options.deployment_profile)
        << " binary_sha256=" << value_or_unknown(options.binary_sha256)
        << " artifact_sha256=" << value_or_unknown(options.artifact_sha256)
        << " config_sha256=" << value_or_unknown(options.config_sha256)
        << " model=" << load.model_id << " weights=" << load.weights_id;
    return out.str();
}

} // namespace ninfer::serve
