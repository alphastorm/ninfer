#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace ninfer::runtime::windows {

class ContinuationCheckpointReadError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#if defined(_WIN32)
[[nodiscard]] std::shared_ptr<ContinuationCheckpointReadQueue>
make_direct_storage_checkpoint_read_queue(const std::filesystem::path& root,
                                          std::uint32_t timeout_ms);
#endif

} // namespace ninfer::runtime::windows
