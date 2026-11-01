#pragma once

#include "ninfer/types.h"
#include "runtime/contract/runtime_fingerprint.h"

namespace ninfer::runtime {

// Pure projection of effective startup options and resolved Program capacities.
inline DurableEngineIdentity engine_identity(const EngineOptions& options,
                                              const MemorySummary& memory) {
    const char* kv;
    switch (options.kv_cache) {
    case KvCacheStorage::BFloat16: kv = "bf16"; break;
    case KvCacheStorage::Int8Group64: kv = "int8"; break;
    case KvCacheStorage::Fp8E4M3Row256: kv = "fp8"; break;
    case KvCacheStorage::Fp8KeyNvfp4Value: kv = "k8v4"; break;
    case KvCacheStorage::Nvfp4Group16: kv = "nvfp4"; break;
    case KvCacheStorage::Rk4V4: kv = "rk4v4"; break;
    }
    const char* backend;
    switch (options.speculative.backend) {
    case SpeculativeBackend::None: backend = "none"; break;
    case SpeculativeBackend::Mtp: backend = "mtp"; break;
    case SpeculativeBackend::DFlash: backend = "dflash"; break;
    case SpeculativeBackend::DFlash2: backend = "dflash2"; break;
    }
    return {.max_context = options.max_context, .kv_capacity = memory.kv_capacity,
            .kv_dtype = kv, .max_concurrency = options.max_concurrency,
            .prefill_chunk = memory.prefill_chunk, .speculative_backend = backend,
            .draft_window = options.speculative.draft_tokens,
            .proposal_head = options.speculative.proposal_head == ProposalHead::Optimized
                ? "optimized" : "full",
            .enable_vision = options.enable_vision, .use_cuda_graph = options.use_cuda_graph};
}

} // namespace ninfer::runtime
