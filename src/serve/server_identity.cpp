#include "serve/server_identity.h"

#include "ninfer/build_info.h"

#include <sstream>

namespace ninfer::serve {
using Json = nlohmann::json;

Json server_identity_json(const ServeOptions& options, const ninfer::LoadSummary& load) {
    const auto build = ninfer::build_info();
    return {{"upstream_base_sha", build.upstream_base_sha}, {"patch_stack_sha", build.patch_stack_sha},
            {"source_dirty", build.source_dirty}, {"build_profile", build.build_profile},
            {"build_type", build.build_type}, {"cxx_compiler", build.cxx_compiler},
            {"cuda_compiler", build.cuda_compiler}, {"cuda_toolkit", build.cuda_toolkit},
            {"cuda_architecture", build.cuda_architecture},
            {"deployment_profile", options.deployment_profile}, {"binary_sha256", options.binary_sha256},
            {"model_artifact_sha256", options.artifact_sha256}, {"config_sha256", options.config_sha256},
            {"target", load.architecture}, {"model_id", resolve_public_model_id(options, load.model_name)},
            {"weights_id", options.artifact_sha256}};
}
std::string format_server_identity(const ServeOptions& options, const ninfer::LoadSummary& load) {
    const auto identity = server_identity_json(options, load);
    std::ostringstream out;
    out << "identity upstream=" << identity.at("upstream_base_sha").get<std::string>()
        << " patch=" << identity.at("patch_stack_sha").get<std::string>()
        << " dirty=" << (identity.at("source_dirty").get<bool>() ? "true" : "false")
        << " build=" << identity.at("build_profile").get<std::string>()
        << " deployment=" << options.deployment_profile << " binary_sha256=" << options.binary_sha256
        << " artifact_sha256=" << options.artifact_sha256 << " config_sha256=" << options.config_sha256
        << " model=" << identity.at("model_id").get<std::string>();
    return out.str();
}

} // namespace ninfer::serve
