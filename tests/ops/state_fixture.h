#pragma once

#include "core/state_image.h"
#include "ops/op_tester.h"

namespace ninfer::test {

// Runs the existing oracle samples through a real StateImage field. Guards occupy the other
// fields and alignment gaps; the original dense buffer remains the oracle's logical ordering.
class SlottedStateFixture {
public:
    SlottedStateFixture(const Tensor& dense, int slot_axis)
        : dense_(dense), slot_axis_(slot_axis), layout_(plan(dense, slot_axis)),
          backing_(layout_.storage.bytes), images_(backing_.span(), layout_),
          tensor(images_.field(StateField{1})) {
        backing_.fill(0xa5);
        upload();
    }

    void upload() const {
        const Tensor& dense = dense_;
        CUDA_CHECK(cudaMemcpy2D(tensor.data, tensor.nb[slot_axis_], dense.data,
                                dense.nb[slot_axis_], dense.nb[slot_axis_], dense.ne[slot_axis_],
                                cudaMemcpyDeviceToDevice));
        cuda_synchronize();
    }

    void copy_back() const {
        cuda_synchronize();
        CUDA_CHECK(cudaMemcpy2D(dense_.data, dense_.nb[slot_axis_], tensor.data,
                                tensor.nb[slot_axis_], dense_.nb[slot_axis_], dense_.ne[slot_axis_],
                                cudaMemcpyDeviceToDevice));
    }

    int verify_guards(std::string_view label) const {
        cuda_synchronize();
        int failures      = backing_.verify_guards(std::string(label).c_str());
        const auto& field = layout_.fields[1];
        for (const auto [offset, bytes] :
             {std::pair<std::size_t, std::size_t>{0, field.tensor.region.offset},
              {field.tensor.region.offset + field.tensor.region.bytes,
               layout_.image_stride - field.tensor.region.offset - field.tensor.region.bytes}}) {
            std::vector<unsigned char> guards(bytes * layout_.slot_count);
            CUDA_CHECK(cudaMemcpy2D(
                guards.data(), bytes, static_cast<const std::byte*>(backing_.data()) + offset,
                layout_.image_stride, bytes, layout_.slot_count, cudaMemcpyDeviceToHost));
            if (std::any_of(guards.begin(), guards.end(),
                            [](auto value) { return value != 0xa5; })) {
                std::cerr << label << ": StateImage neighbouring field overwritten\n";
                ++failures;
            }
        }
        return failures;
    }

private:
    static StateImageLayout plan(const Tensor& dense, int slot_axis) {
        StateImageBuilder image;
        (void)image.add(DType::I32, {3}, 1, "preceding field");
        if (slot_axis == 1)
            (void)image.add(dense.dtype, {dense.ne[0]}, 1, "state");
        else if (slot_axis == 2)
            (void)image.add(dense.dtype, {dense.ne[0], dense.ne[1]}, 1, "state");
        else if (slot_axis == 3)
            (void)image.add(dense.dtype, {dense.ne[0], dense.ne[1], dense.ne[2]}, 1, "state");
        else
            throw std::invalid_argument("invalid fixture slot axis");
        (void)image.add(DType::BF16, {7}, 1, "following field");
        LayoutBuilder backing;
        return image.finish(backing, dense.ne[slot_axis]);
    }

    Tensor dense_;
    int slot_axis_;
    StateImageLayout layout_;
    GuardedDeviceBuffer backing_;
    StateImageStorage images_;

public:
    Tensor tensor;
};

} // namespace ninfer::test
