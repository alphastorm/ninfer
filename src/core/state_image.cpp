#include "core/state_image.h"

#include "core/transfer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ninfer {
namespace {

constexpr std::size_t kAlignment = 256;

std::size_t product(std::size_t a, std::size_t b) {
    if (b != 0 && a > static_cast<std::size_t>(INT64_MAX) / b) {
        throw std::overflow_error("StateImage extent exceeds int64");
    }
    return a * b;
}

void clear_memory(MemorySpan memory, CudaExecutionView execution) {
    if (memory.device != execution.device()) {
        throw std::invalid_argument("StateImage clearing requires its CUDA execution device");
    }
    CudaDeviceScope scope(execution.device());
    CUDA_CHECK(cudaMemsetAsync(memory.data, 0, memory.bytes, execution.stream()));
}

} // namespace

TransferWork StateImageLayout::transfer_work() const noexcept {
    return {.payload_bytes = image_stride, .copy_operations = 1};
}

StateField StateImageBuilder::add(DType dtype, std::initializer_list<std::int32_t> shape,
                                  std::uint32_t layers, std::string_view label) {
    if (shape.size() == 0 || shape.size() > 3 || layers == 0 || fields_.size() >= UINT32_MAX ||
        std::any_of(shape.begin(), shape.end(), [](auto extent) { return extent <= 0; })) {
        throw std::invalid_argument(
            "StateImage field requires 1..3 inner axes and positive layers");
    }
    LayoutBuilder field;
    TensorRegion tensor  = field.add_tensor(dtype, shape, kAlignment, label);
    const auto stride    = field.finish(kAlignment, label);
    const auto region    = image_.add(product(stride, layers), kAlignment, label);
    tensor.region.offset = region.offset;
    const StateField id{static_cast<std::uint32_t>(fields_.size())};
    fields_.push_back({tensor, layers, stride, static_cast<int>(shape.size())});
    return id;
}

LayoutRegion StateImageBuilder::group(StateField first, StateField last) const {
    if (first.index > last.index || last.index >= fields_.size()) {
        throw std::out_of_range("StateImage field group is out of range");
    }
    const auto begin = fields_[first.index].tensor.region.offset;
    const auto& end  = fields_[last.index];
    return {begin, end.tensor.region.offset + product(end.layer_stride, end.layers) - begin,
            kAlignment};
}

StateImageLayout StateImageBuilder::finish(LayoutBuilder& backing, std::int32_t slots) const {
    if (slots <= 0 || fields_.empty()) {
        throw std::invalid_argument("StateImage requires fields and positive slot capacity");
    }
    const auto stride = image_.finish(kAlignment, "StateImage");
    return {backing.add(product(stride, slots), kAlignment, "StateImage storage"), stride, slots,
            fields_};
}

StateImageStorage::StateImageStorage(MemorySpan backing, const StateImageLayout& layout)
    : layout_(layout), storage_(layout.storage.bind(backing)) {
    if (layout.slot_count <= 0 || layout.image_stride == 0 || layout.fields.empty() ||
        storage_.bytes != product(layout.image_stride, layout.slot_count)) {
        throw std::invalid_argument("StateImage storage geometry is inconsistent");
    }
    std::size_t end = 0;
    for (const auto& field : layout.fields) {
        if (field.slot_axis < 1 || field.slot_axis > 3 || field.layers == 0 ||
            field.tensor.region.offset < end || field.layer_stride < field.tensor.region.bytes) {
            throw std::invalid_argument("StateImage fields overlap or have invalid geometry");
        }
        const auto extent = product(field.layer_stride, field.layers);
        if (field.tensor.region.offset > layout.image_stride ||
            extent > layout.image_stride - field.tensor.region.offset) {
            throw std::invalid_argument("StateImage field exceeds its image");
        }
        // TensorRegion checks dtype, shape, alignment and the addressed inner payload.
        const Tensor tensor = field.tensor.bind(storage_.subspan(0, layout.image_stride));
        for (int axis = field.slot_axis; axis < 4; ++axis) {
            if (tensor.ne[axis] != 1) {
                throw std::invalid_argument("StateImage field uses the reserved slot axis");
            }
        }
        if (field.layer_stride % tensor.nb[0] != 0 || layout.image_stride % tensor.nb[0] != 0) {
            throw std::invalid_argument("StateImage strides split a field element");
        }
        end = field.tensor.region.offset + extent;
    }
}

Tensor StateImageStorage::field(StateField id, std::uint32_t layer) const {
    const auto& field = layout_.fields.at(id.index);
    if (layer >= field.layers) { throw std::out_of_range("StateImage layer is out of range"); }
    Tensor tensor              = field.tensor.bind(storage_.subspan(layer * field.layer_stride));
    tensor.ne[field.slot_axis] = slot_count();
    for (int axis = field.slot_axis; axis < 4; ++axis) {
        tensor.nb[axis] = static_cast<std::int64_t>(axis == field.slot_axis ? layout_.image_stride
                                                                            : storage_.bytes);
    }
    return tensor;
}

std::int64_t StateImageStorage::layer_stride(StateField field) const {
    return static_cast<std::int64_t>(layout_.fields.at(field.index).layer_stride);
}

MemorySpan StateImageStorage::image(std::int32_t slot) const {
    if (slot < 0 || slot >= slot_count()) {
        throw std::out_of_range("StateImage slot out of range");
    }
    return storage_.subspan(static_cast<std::size_t>(slot) * image_bytes(), image_bytes());
}

void StateImageStorage::clear(std::int32_t slot, CudaExecutionView execution) const {
    clear_memory(image(slot), execution);
}

void StateImageStorage::clear_all(CudaExecutionView execution) const {
    clear_memory(storage_, execution);
}

void StateImageStorage::copy(std::int32_t source, std::int32_t destination,
                             CudaExecutionView execution) const {
    copy_bytes_async(image(destination), image(source), execution);
}

void StateImageStorage::copy_group(LayoutRegion group, std::int32_t source,
                                   std::int32_t destination, CudaExecutionView execution) const {
    copy_bytes_async(group.bind(image(destination)), group.bind(image(source)), execution);
}

void StateImageStorage::copy_to_host(std::int32_t source, MemorySpan destination,
                                     CudaExecutionView execution) const {
    if (destination.device != Device::cpu()) {
        throw std::invalid_argument("StateImage Host destination must be CPU memory");
    }
    copy_bytes_async(destination, image(source), execution);
}

void StateImageStorage::copy_from_host(ConstMemorySpan source, std::int32_t destination,
                                       CudaExecutionView execution) const {
    if (source.device != Device::cpu()) {
        throw std::invalid_argument("StateImage Host source must be CPU memory");
    }
    copy_bytes_async(image(destination), source, execution);
}

} // namespace ninfer
