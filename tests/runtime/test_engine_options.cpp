#include "runtime/engine/model_construction.h"
#include "runtime/engine/prefix_reuse_path.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>

int main() {
    try {
        using Role = ninfer::runtime::CheckpointRole;
        using Path = ninfer::PrefixReusePath;
        for (const auto& [role, path] : std::array{
                 std::pair{Role::Continuation, Path::PrivateEndpoint},
                 std::pair{Role::InputReplay, Path::PrivateInputReplay},
                 std::pair{Role::LongAnchor, Path::PrivateLongAnchor},
                 std::pair{Role::SharedPrefix, Path::SharedStablePrefix}}) {
            if (ninfer::runtime::checkpoint_reuse_path(role) != path) {
                throw std::runtime_error("semantic checkpoint role was misclassified");
            }
        }
        for (const auto storage : std::array{
                 ninfer::KvCacheStorage::BFloat16, ninfer::KvCacheStorage::Int8Group64,
                 ninfer::KvCacheStorage::Fp8E4M3Row256, ninfer::KvCacheStorage::Nvfp4Group16,
                 ninfer::KvCacheStorage::Fp8KeyNvfp4Value}) {
            ninfer::EngineOptions options;
            options.kv_cache = storage;
            options.max_concurrency = 2;
            bool supported = true;
#if defined(NINFER_SM86) || defined(NINFER_SM89)
            supported = storage != ninfer::KvCacheStorage::Nvfp4Group16 &&
                        storage != ninfer::KvCacheStorage::Fp8KeyNvfp4Value;
#endif
#if defined(NINFER_SM86)
            supported = supported && storage != ninfer::KvCacheStorage::Fp8E4M3Row256;
#endif
            bool rejected = false;
            try {
                const auto normalized = ninfer::runtime::normalize_engine_options(options);
                if (normalized.kv_cache != storage || normalized.max_concurrency != 2 ||
                    normalized.pending_timeout_ms != 30000 || normalized.gpu_keep_warm_ms != 0) {
                    throw std::runtime_error("normalization changed a supported product option");
                }
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (rejected == supported) {
                throw std::runtime_error("compile-profile KV capability was not enforced before startup");
            }
        }
        std::cout << "ok: profile-specific KV validation before CUDA/artifact allocation\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
