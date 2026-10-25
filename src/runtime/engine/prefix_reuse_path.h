#pragma once

#include "ninfer/types.h"
#include "runtime/contract/resources.h"

#include <stdexcept>

namespace ninfer::runtime {

[[nodiscard]] inline PrefixReusePath checkpoint_reuse_path(CheckpointRole role) {
    switch (role) {
    case CheckpointRole::Continuation: return PrefixReusePath::PrivateEndpoint;
    case CheckpointRole::InputReplay: return PrefixReusePath::PrivateInputReplay;
    case CheckpointRole::LongAnchor: return PrefixReusePath::PrivateLongAnchor;
    case CheckpointRole::SharedPrefix: return PrefixReusePath::SharedStablePrefix;
    }
    throw std::logic_error("checkpoint has an invalid semantic role");
}

} // namespace ninfer::runtime
