#pragma once

#include "ninfer/types.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace ninfer::product {

[[nodiscard]] inline DFlashMode parse_dflash_mode(std::string_view value) {
    if (value == "off") return DFlashMode::Off;
    if (value == "adaptive") return DFlashMode::Adaptive;
    if (value == "force") return DFlashMode::ForcedCap;
    if (value == "calibrate") return DFlashMode::Calibrate;
    throw std::invalid_argument("invalid DFlash governor mode");
}

inline void validate_dflash_options(const DFlashOptions& options,
                                    const SpeculativeOptions& startup) {
    if (options.mode != DFlashMode::Off && options.mode != DFlashMode::Adaptive &&
        options.mode != DFlashMode::ForcedCap && options.mode != DFlashMode::Calibrate)
        throw std::invalid_argument("invalid DFlash governor mode");
    if ((options.mode != DFlashMode::Off || options.trace) &&
        startup.backend != SpeculativeBackend::DFlash2)
        throw std::invalid_argument("DFlash measurement options require --spec dflash2");
    if (options.mode != DFlashMode::Off && startup.draft_tokens != 7)
        throw std::invalid_argument("ExtentCap requires physical K7/W8");
    if (options.rounds != 8 && options.rounds != 16 && options.rounds != 32)
        throw std::invalid_argument("DFlash R must be 8, 16 or 32");
    bool threshold_valid = false;
    for (std::uint32_t q = 10; q <= 13; ++q)
        threshold_valid |= options.threshold == options.rounds * q / 4;
    if (!threshold_valid || options.cap < 1 || options.cap > 3)
        throw std::invalid_argument("DFlash H/cap is outside the preregistered grid");
    if (options.mode == DFlashMode::Calibrate &&
        (options.rounds != 16 || options.threshold != 44 || options.cap != 3))
        throw std::invalid_argument("CAL fixes R16/H44/cap3");
}

inline void validate_dflash_trace_sha(std::string_view sha) {
    if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        throw std::invalid_argument("DFlash trace requires a canonical request SHA-256");
}

[[nodiscard]] inline SpeculativeBackend parse_speculative_backend(std::string_view value) {
    if (value == "mtp") { return SpeculativeBackend::Mtp; }
    if (value == "dflash") { return SpeculativeBackend::DFlash; }
    if (value == "dflash2") { return SpeculativeBackend::DFlash2; }
    throw std::invalid_argument("invalid speculative backend: " + std::string(value));
}

[[nodiscard]] inline const char* speculative_backend_name(SpeculativeBackend backend) noexcept {
    switch (backend) {
    case SpeculativeBackend::None:
        return "none";
    case SpeculativeBackend::Mtp:
        return "mtp";
    case SpeculativeBackend::DFlash:
        return "dflash";
    case SpeculativeBackend::DFlash2:
        return "dflash2";
    }
    return "unknown";
}

inline void validate_speculative_cli_options(const SpeculativeOptions& options) {
    switch (options.backend) {
    case SpeculativeBackend::None:
        if (options.draft_tokens != 0 || options.proposal_head != ProposalHead::Full) {
            throw std::invalid_argument(
                "--draft-tokens and --lm-head-draft require --spec mtp|dflash|dflash2");
        }
        return;
    case SpeculativeBackend::Mtp:
        if (options.draft_tokens == 0 || options.draft_tokens > 5) {
            throw std::invalid_argument("--spec mtp requires --draft-tokens in [1,5]");
        }
        return;
    case SpeculativeBackend::DFlash:
        if (options.draft_tokens == 0 || options.draft_tokens > 15) {
            throw std::invalid_argument("--spec dflash requires --draft-tokens in [1,15]");
        }
        return;
    case SpeculativeBackend::DFlash2:
        if (options.draft_tokens == 0 || options.draft_tokens > 15) {
            throw std::invalid_argument("--spec dflash2 requires --draft-tokens in [1,15]");
        }
        return;
    }
    throw std::invalid_argument("invalid speculative backend");
}

} // namespace ninfer::product
