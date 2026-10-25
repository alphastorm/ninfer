#pragma once

#include "core/device.h"
#include "core/tensor.h"

namespace ninfer::ops {

// Overlay selected rows of a sparse speculative proposal with deterministic copies.
// copies/drafts are I32 [K,B], copy_rows is I32 [B], candidate_ids is I32 [16,K,B],
// proposal_q is FP32 [16,K,B]; all are dense on execution.device(), 1<=K<=15, 1<=B<=2.
// A nonzero copy_rows[b] writes drafts[j,b]=copies[j,b], candidate 0 equal to it with
// q=1, and candidates s=1..15 equal to (copies[j,b]+s)%token_domain with q=0.
// Other rows are unchanged. token_domain>=16 and every selected copy token is in
// [0,token_domain). Inputs and outputs must be mutually nonaliasing. No workspace.
// This only supplies q; target sampling/acceptance and its RNG are unchanged.
void overlay_copy_proposals(const Tensor& copies, const Tensor& copy_rows,
                             std::int32_t token_domain, Tensor& drafts,
                             Tensor& candidate_ids, Tensor& proposal_q,
                             CudaExecutionView execution);

} // namespace ninfer::ops
