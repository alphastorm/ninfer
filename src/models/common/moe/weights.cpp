#include "models/common/moe/weights.h"

namespace ninfer::models::moe {
namespace {

artifact::Part region(const artifact::ParameterReference& reference) {
    if (reference.binding.parts.empty()) { throw artifact::ArtifactError("empty expert Binding"); }
    auto out = reference.binding.parts.front();
    for (std::size_t i = 1; i < reference.binding.parts.size(); ++i) {
        const auto& next = reference.binding.parts[i];
        if (next.object != out.object || next.begin != out.end) {
            throw artifact::ArtifactError("expert matrix requires contiguous parent rows");
        }
        out.end = next.end;
    }
    return out;
}

} // namespace

BankDescription describe_bank(const artifact::Reader& reader,
                              std::span<const ExpertBinding> experts) {
    if (experts.empty()) throw artifact::ArtifactError("empty expert bank");
    const auto& gate     = experts.front().gate;
    const auto& down     = experts.front().down;
    const auto gu_origin = region(gate), down_origin = region(down);
    const auto n = gate.shape.at(0), k = gate.shape.at(1);
    const auto count = static_cast<std::uint32_t>(experts.size());
    const auto check = [&](const artifact::ParameterReference& reference,
                           artifact::ObjectHandle object, std::uint64_t begin, std::uint64_t rows,
                           std::uint64_t columns) {
        const auto part = region(reference);
        if (reference.shape != artifact::Shape{rows, columns} || part.object != object ||
            part.begin != begin || part.end != begin + rows * columns)
            throw artifact::ArtifactError("expert Bindings do not form an affine bank");
    };
    for (std::size_t e = 0; e < experts.size(); ++e) {
        check(experts[e].gate, gu_origin.object, gu_origin.begin + e * 2 * n * k, n, k);
        check(experts[e].up, gu_origin.object, gu_origin.begin + (e * 2 + 1) * n * k, n, k);
        check(experts[e].down, down_origin.object, down_origin.begin + e * n * k, k, n);
    }
    if (gu_origin.begin % k || down_origin.begin % n)
        throw artifact::ArtifactError("expert Binding splits a row");
    return {{gu_origin.object, describe_weight_bank(reader.geometry(gu_origin.object),
                                                    {gu_origin.begin / k, count, 2 * n, 2 * n})},
            {down_origin.object, describe_weight_bank(reader.geometry(down_origin.object),
                                                      {down_origin.begin / n, count, k, k})}};
}

BankDescription describe_bank(const artifact::Reader& reader, const ExpertBinding& bank) {
    const auto& shape = bank.gate.shape;
    if (shape.size() != 3 || bank.up.shape != shape ||
        bank.down.shape != artifact::Shape{shape[0], shape[2], shape[1]} || !shape[0] ||
        shape[0] > UINT32_MAX || !shape[1] || !shape[2] || bank.gate.binding.parts.empty() ||
        bank.down.binding.parts.empty())
        throw artifact::ArtifactError("invalid rank-3 expert bank geometry");
    const auto count = static_cast<std::uint32_t>(shape[0]);
    const auto n = shape[1], k = shape[2];
    const auto width = artifact::checked_mul(n, k, "expert matrix elements");
    const auto gu = bank.gate.binding.parts.front(), down = bank.down.binding.parts.front();
    const auto check = [&](const artifact::ParameterReference& ref, artifact::ObjectHandle object,
                           std::uint64_t origin, std::uint64_t stride) {
        std::size_t part_index = 0;
        std::uint64_t consumed = 0;
        for (std::uint32_t e = 0; e < count; ++e) {
            auto cursor = artifact::checked_add(
                origin, artifact::checked_mul(e, stride, "bank stride"), "bank origin");
            auto remaining = width;
            while (remaining) {
                if (part_index == ref.binding.parts.size())
                    throw artifact::ArtifactError("expert bank Binding is incomplete");
                const auto& part = ref.binding.parts[part_index];
                if (part.object != object || part.begin + consumed != cursor || part.end <= cursor)
                    throw artifact::ArtifactError("expert Bindings do not form an affine bank");
                const auto take = std::min(remaining, part.end - cursor);
                remaining -= take;
                cursor += take;
                consumed += take;
                if (cursor == part.end) {
                    ++part_index;
                    consumed = 0;
                }
            }
        }
        if (part_index != ref.binding.parts.size() || consumed)
            throw artifact::ArtifactError("expert bank Binding exceeds its geometry");
    };
    const auto stride = artifact::checked_mul(2, width, "gate/up stride");
    check(bank.gate, gu.object, gu.begin, stride);
    check(bank.up, gu.object, artifact::checked_add(gu.begin, width, "up origin"), stride);
    check(bank.down, down.object, down.begin, width);
    if (gu.begin % k || down.begin % n) throw artifact::ArtifactError("expert bank splits a row");
    return {{gu.object,
             describe_weight_bank(reader.geometry(gu.object), {gu.begin / k, count, 2 * n, 2 * n})},
            {down.object,
             describe_weight_bank(reader.geometry(down.object), {down.begin / n, count, k, k})}};
}

std::vector<ExpertBankSource> bind_banks(std::span<const BankDescription> banks,
                                         const artifact::MaterializedArtifact& backing,
                                         std::span<const Device> devices) {
    std::vector<ExpertBankSource> out;
    out.reserve(banks.size());
    if (devices.size() != banks.size()) {
        throw artifact::ArtifactError("expert placement count differs");
    }
    for (std::size_t i = 0; i < banks.size(); ++i) {
        const auto& bank  = banks[i];
        const auto device = devices[i];
        const auto bind   = [&](const MatrixSource& source) {
            const auto& parent = device.type() == DeviceType::CPU
                                       ? backing.host_parent(source.object)
                                       : backing.device_parent(source.object);
            if (parent.device != device) {
                throw artifact::ArtifactError("expert source device differs");
            }
            return bind_weight_bank(parent, source.layout);
        };
        const auto upload = [&](const MatrixSource& source) {
            return device == Device::cpu() ? backing.host_transfer_view(source.object)
                                           : CudaHostView{};
        };
        out.push_back(
            {bind(bank.gate_up), bind(bank.down), upload(bank.gate_up), upload(bank.down)});
    }
    return out;
}

} // namespace ninfer::models::moe
