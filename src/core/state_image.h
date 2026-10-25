#pragma once

#include "core/device.h"
#include "core/layout.h"
#include "core/transfer_work.h"

#include <cstdint>
#include <vector>

namespace ninfer {

struct StateField {
    std::uint32_t index = 0;
};

struct StateFieldLayout {
    TensorRegion tensor;
    std::uint32_t layers     = 1;
    std::size_t layer_stride = 0;
    int slot_axis            = 0;
};

struct StateImageLayout {
    LayoutRegion storage;
    std::size_t image_stride = 0;
    std::int32_t slot_count  = 0;
    std::vector<StateFieldLayout> fields;

    [[nodiscard]] TransferWork transfer_work() const noexcept;
};

// Declares one image. Each field has dense inner dimensions and equally spaced layers.
// Slots repeat the complete image; Device and Host images use the same internal offsets.
class StateImageBuilder {
public:
    [[nodiscard]] StateField add(DType dtype, std::initializer_list<std::int32_t> shape,
                                 std::uint32_t layers, std::string_view label);
    [[nodiscard]] LayoutRegion group(StateField first, StateField last) const;
    [[nodiscard]] StateImageLayout finish(LayoutBuilder& backing, std::int32_t slots) const;

private:
    LayoutBuilder image_;
    std::vector<StateFieldLayout> fields_;
};

// Borrows caller-owned backing. Slot identity, validity and asynchronous lifetimes belong to
// the owner; no allocation or initialization occurs when the storage is bound.
class StateImageStorage {
public:
    StateImageStorage(MemorySpan backing, const StateImageLayout& layout);

    [[nodiscard]] const StateImageLayout& layout() const noexcept { return layout_; }

    [[nodiscard]] std::int32_t slot_count() const noexcept { return layout_.slot_count; }

    [[nodiscard]] std::size_t image_bytes() const noexcept { return layout_.image_stride; }

    [[nodiscard]] Tensor field(StateField field, std::uint32_t layer = 0) const;
    [[nodiscard]] std::int64_t layer_stride(StateField field) const;
    [[nodiscard]] MemorySpan image(std::int32_t slot) const;

    void clear(std::int32_t slot, CudaExecutionView execution) const;
    void clear_all(CudaExecutionView execution) const;
    void copy(std::int32_t source, std::int32_t destination, CudaExecutionView execution) const;
    void copy_group(LayoutRegion group, std::int32_t source, std::int32_t destination,
                    CudaExecutionView execution) const;
    void copy_to_host(std::int32_t source, MemorySpan destination,
                      CudaExecutionView execution) const;
    void copy_from_host(ConstMemorySpan source, std::int32_t destination,
                        CudaExecutionView execution) const;

private:
    StateImageLayout layout_;
    MemorySpan storage_;
};

} // namespace ninfer
