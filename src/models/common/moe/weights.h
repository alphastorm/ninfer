#pragma once

#include "artifact/binder.h"
#include "artifact/materializer.h"
#include "core/weight_bank.h"

namespace ninfer::models::moe {

struct ExpertBinding {
    const artifact::ParameterReference& gate;
    const artifact::ParameterReference& up;
    const artifact::ParameterReference& down;
};

struct MatrixSource {
    artifact::ObjectHandle object;
    WeightBankLayout layout;
};

struct BankDescription {
    MatrixSource gate_up, down;
};

// Logical expert order must form affine gate/up and down banks in the stored parents.
[[nodiscard]] BankDescription describe_bank(const artifact::Reader&,
                                            std::span<const ExpertBinding>);
// Three rank-3 logical [E,I,H], [E,I,H], [E,H,I] bindings over affine stored banks.
[[nodiscard]] BankDescription describe_bank(const artifact::Reader&, const ExpertBinding&);
[[nodiscard]] std::vector<ExpertBankSource> bind_banks(std::span<const BankDescription>,
                                                       const artifact::MaterializedArtifact&,
                                                       std::span<const Device>);

} // namespace ninfer::models::moe
