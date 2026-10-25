#pragma once

#include "core/buffer.h"
#include "options.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ninfer::bench::qsa {

inline std::vector<int> broadcast(std::vector<int> values, int batch) {
    if (values.size() == 1) values.resize(batch, values[0]);
    if (values.size() != std::size_t(batch)) throw std::invalid_argument("metadata must match B");
    return values;
}

inline std::string csv_list(const std::vector<int>& values) {
    std::string result;
    for (int value : values) {
        if (!result.empty()) result += ':';
        result += std::to_string(value);
    }
    return result;
}

template <class Word>
struct StatePlane {
    DeviceBuffer storage;
    Tensor view;
    std::vector<Word> host;
    std::size_t leading = 0;

    StatePlane(int features, int count, int outer, DType dtype, const std::string& layout,
               bool state_coordinates = false)
        : view(Tensor::metadata(dtype, {features, count, outer})) {
        if (layout == "layered") {
            const int axis = state_coordinates ? 1 : 2;
            view.nb[axis] *= 12;
        } else if (layout == "unaligned" && dtype == DType::BF16) {
            leading    = 1;
            view.nb[1] = (features + 1) * sizeof(Word);
            view.nb[2] = ((features + 1) * count + 1) * sizeof(Word);
        }
        const auto words = leading + features + (count - 1) * (view.nb[1] / sizeof(Word)) +
                           (outer - 1) * (view.nb[2] / sizeof(Word));
        host.resize(words);
        for (std::size_t i = 0; i < words; ++i) host[i] = Word((i * 37 + 101) & 0x7fff);
        storage     = DeviceBuffer(ninfer::bench::device_context().device(), words * sizeof(Word));
        view.data   = static_cast<Word*>(storage.data()) + leading;
        view.device = storage.device();
    }

    Word& at(int feature, int column, int outer = 0) {
        return host[leading + feature + std::size_t(column) * (view.nb[1] / sizeof(Word)) +
                    std::size_t(outer) * (view.nb[2] / sizeof(Word))];
    }

    void upload() { storage.copy_from_host(host.data(), host.size() * sizeof(Word)); }
};

} // namespace ninfer::bench::qsa
