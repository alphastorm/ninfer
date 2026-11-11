// GPU-free execution of the real StateImageStore. CUDA entry points are interposed;
// ownership, reservations, epochs and replica publication are native.
#include "core/device.h"
#include "cuda_interpose.h"
#include "models/common/context/state_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::size_t copies = 0, synchronizations = 0, fail_copy = 0, error_strings = 0, rows_2d = 0;
}

// Expand CUDA's versioned API aliases before the Linux linker-wrap token is formed.
#define NINFER_EXPORT_STATE_CUDA(name) NINFER_TEST_CUDA_WRAP(name)

extern "C" {
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaGetDeviceCount)(int* count) {
    *count = 1;
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaGetDevice)(int* device) {
    *device = 0;
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaSetDevice)(int) { return cudaSuccess; }
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaSetDeviceFlags)(unsigned) { return cudaSuccess; }
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaGetDeviceProperties)(cudaDeviceProp* props, int) {
    *props = {};
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaStreamCreateWithFlags)(cudaStream_t* stream,
                                                                                unsigned) {
    static std::array<unsigned char, 2> streams{};
    static std::size_t created = 0;
    *stream = reinterpret_cast<cudaStream_t>(&streams[created++ % streams.size()]);
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaStreamDestroy)(cudaStream_t) { return cudaSuccess; }
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaMallocHost)(void** memory, std::size_t bytes) {
    *memory = ::operator new(bytes, std::align_val_t{256}, std::nothrow);
    return *memory ? cudaSuccess : cudaErrorMemoryAllocation;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaFreeHost)(void* memory) {
    ::operator delete(memory, std::align_val_t{256});
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaMemcpyAsync)(void* to, const void* from,
    std::size_t bytes, cudaMemcpyKind, cudaStream_t) {
    if (++copies == fail_copy) return cudaErrorInvalidValue;
    std::memcpy(to, from, bytes);
    return cudaSuccess;
}
// Device-staged export rows: the exporter's D2H pitched copies, emulated over host backing.
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaMemcpy2DAsync)(void* to, std::size_t to_pitch,
    const void* from, std::size_t from_pitch, std::size_t width, std::size_t rows, cudaMemcpyKind,
    cudaStream_t) {
    ++rows_2d;
    for (std::size_t row = 0; row < rows; ++row)
        std::memcpy(static_cast<std::byte*>(to) + row * to_pitch,
                    static_cast<const std::byte*>(from) + row * from_pitch, width);
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaStreamSynchronize)(cudaStream_t) {
    ++synchronizations;
    return cudaSuccess;
}
// Error formatting is part of the interposed CUDA boundary too: the injected copy error
// must throw through native transfer code without entering an uninitialized Windows runtime.
const char* CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaGetErrorString)(cudaError_t) {
    ++error_strings;
    return "injected host-test CUDA copy failure";
}
}

namespace {
using namespace ninfer;
using Store = models::context::StateImageStore;
using HostKind = Store::HostPreparation::Kind;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

StateImageLayout layout() {
    StateImageBuilder schema;
    (void)schema.add(DType::FP32, {4, 4}, 1, "recurrent");
    (void)schema.add(DType::BF16, {8}, 1, "hidden");
    (void)schema.add(DType::BF16, {4, 8}, 1, "draft_ring");
    LayoutBuilder backing;
    return schema.finish(backing, 3);
}

struct Fixture {
    alignas(256) std::array<std::byte, 4096> backing{};
    StateImageLayout schema = layout();
    StateImageStorage physical;
    HostContextArena host;
    Store images;
    std::array<Store::Handle, 3> sources;
    std::array<std::uint64_t, 3> epochs;

    Fixture(Device device, std::size_t host_slots)
        : physical({backing.data(), backing.size(), device}, schema),
          host(schema.image_stride * host_slots, schema.image_stride),
          images(physical, &host, 8) {
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto source = images.reserve_destination();
            require(source.has_value(), "source allocation");
            sources[i] = *source;
            const auto bytes = physical.image(images.physical_slot(*source));
            std::memset(bytes.data, static_cast<int>(i + 1), bytes.bytes);
            images.complete_initialization(*source);
            images.freeze(*source);
            images.retain_checkpoint_reference(*source);
            epochs[i] = images.content_epoch(*source);
        }
    }

    void unchanged_device_sources() {
        require(images.device_occupied() == 3 && images.host_occupied() == 0,
                "refusal leaves original Device residency");
        for (std::size_t i = 0; i < sources.size(); ++i) {
            require(images.content_epoch(sources[i]) == epochs[i] &&
                        images.source_pins(sources[i]) == 0 &&
                        images.checkpoint_references(sources[i]) == 1,
                    "refusal retains exact content and owners without leaked readers");
        }
    }

    // A staged export's readers resolve every source from its retained Device replica.
    void staged_device_readers() {
        require(images.device_occupied() == 3 && images.host_occupied() == 0,
                "Device-staged export keeps every Device replica and reserves no Host replica");
        std::vector<Store::ReadLease> readers;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            readers.push_back(images.read(sources[i]));
            require(readers.back().epoch() == epochs[i] &&
                        readers.back().device_slot() == images.physical_slot(sources[i]) &&
                        images.source_pins(sources[i]) == 1,
                    "Device-staged reader pins the original Device content");
        }
        readers.clear();
        unchanged_device_sources();
    }
};

// Detected so that the predecessor reports its refusal instead of failing to compile.
template <class Images>
Store::HostPreparation staged_after_refusal(Images& images, std::span<const Store::Handle> sources) {
    if constexpr (requires { images.prepare_device_export(sources); }) {
        return images.prepare_device_export(sources);
    } else {
        throw std::runtime_error("Host shortage stages State images from Device: export refused");
    }
}

template <class Preparation>
std::uint32_t staged_images(const Preparation& preparation) {
    if constexpr (requires { preparation.device_images; }) {
        return preparation.device_images;
    } else {
        return 0;
    }
}

void export_leased_fork(DeviceContext& device) {
    Fixture f(device.device(), 3);
    std::vector<Store::ReadLease> exports;
    for (auto source : f.sources) exports.push_back(f.images.read(source));
    // Reproduce the old export sequence: three available slots are all retained for disk I/O.
    require(!f.images.reserve_destination(), "old Device export readers reproduce fork starvation");
    require(!f.images.drop_device_replica(f.sources[0]), "reader ownership guard remains enforced");
    exports.clear();

    const auto before = copies;
    require(f.images.prepare_host_export(f.sources, device.transfer_view()).kind == HostKind::Prepared,
            "prepare Host export");
    require(copies == before + 3 && f.images.device_occupied() == 0 &&
                f.images.host_occupied() == 3, "one copy per missing Host replica frees all slots");
    for (std::size_t i = 0; i < f.sources.size(); ++i) {
        exports.push_back(f.images.read(f.sources[i]));
        require(exports.back().epoch() == f.epochs[i], "export retains the original content epoch");
    }
    require(f.images.prepare_host_export(f.sources, device.transfer_view()).kind == HostKind::Prepared && copies == before + 3,
            "overlapping Host export readers require no additional copies");
    const auto destination = f.images.reserve_logical_destination();
    require(destination.has_value(), "independent fork identity reservation");
    auto restore = f.images.reserve_host_to_device(f.sources[0], *destination);
    require(restore.has_value(), "export-leased source plus fork must not wait for export retirement");
    restore->enqueue(device.transfer_view());
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
    restore->publish(true);
    f.images.complete_initialization(*destination);
    const auto bytes = f.physical.image(f.images.physical_slot(*destination));
    const auto* restored = static_cast<const std::byte*>(bytes.data);
    require(std::all_of(restored, restored + bytes.bytes, [](auto b) { return b == std::byte{1}; }),
            "fork restores every State byte including draft ring and hidden features");
    std::memset(bytes.data, 0x7f, bytes.bytes); // The fork can now write its own State image.
    for (std::size_t i = 0; i < exports.size(); ++i) {
        const auto host = exports[i].host_view();
        const auto* data = static_cast<const std::byte*>(host.data);
        require(std::all_of(data, data + host.bytes, [i](auto b) { return b == std::byte(i + 1); }),
                "fork writes cannot alter export bytes");
        require(f.images.release_checkpoint_owner(f.sources[i]) && f.images.valid(f.sources[i]),
                "export reader keeps content alive after its last owner retires");
    }
    exports.clear();
    for (auto source : f.sources) require(!f.images.valid(source), "last reader releases orphan");
    require(f.images.release(*destination) && f.images.occupied() == 0,
            "export and fork leave no logical or physical allocations");
}

void existing_host_replica(DeviceContext& device) {
    Fixture f(device.device(), 3);
    auto backup = f.images.reserve_device_to_host(f.sources[0]);
    require(backup.has_value(), "Both source backup");
    backup->enqueue(device.transfer_view());
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
    backup->publish(true);
    const auto before = copies;
    require(f.images.prepare_host_export(f.sources, device.transfer_view()).kind == HostKind::Prepared &&
                copies == before + 2 && f.images.device_occupied() == 0,
            "existing Host replica is reused and its Device replica is released");
}

void failure_recovery(DeviceContext& device) {
    for (const auto slots : {0U, 2U}) {
        Fixture f(device.device(), slots);
        const auto before = copies;
        const auto result = f.images.prepare_host_export(f.sources, device.transfer_view());
        require(result.kind == HostKind::HostCapacity &&
                    result.shortage_bytes == (3 - slots) * f.schema.image_stride && copies == before &&
                    f.host.reserved_bytes() == 0,
                "all Host capacity is reserved before any copy");
        f.unchanged_device_sources();
        // E89: the same shortage still prepares the export through the Device route.
        const auto host_free = f.host.free_bytes();
        const auto staged = staged_after_refusal(f.images, f.sources);
        require(staged.kind == HostKind::Prepared && staged_images(staged) == 3 && copies == before &&
                    f.host.reserved_bytes() == 0 && f.host.free_bytes() == host_free,
                "Host capacity shortage prepares a Device-staged export without Host reservation");
        f.staged_device_readers();
    }
    Fixture f(device.device(), 3);
    {
        auto reader = f.images.read(f.sources[1]);
        const auto before = copies;
        const auto result = f.images.prepare_host_export(f.sources, device.transfer_view());
        require(result.kind == HostKind::Busy && result.shortage_bytes == 0 && copies == before,
                "an existing Device reader is never invalidated to prepare export");
    }
    f.unchanged_device_sources();
    const auto before_sync = synchronizations;
    const auto before_errors = error_strings;
    fail_copy = copies + 2;
    bool failed = false;
    try {
        (void)f.images.prepare_host_export(f.sources, device.transfer_view());
    } catch (const std::runtime_error&) {
        failed = true;
    }
    fail_copy = 0;
    require(failed && error_strings == before_errors + 1 && synchronizations > before_sync,
            "partial enqueue failure settles submitted copies before reservation rollback");
    f.unchanged_device_sources();
    require(f.images.prepare_host_export(f.sources, device.transfer_view()).kind == HostKind::Prepared &&
                f.images.device_occupied() == 0 && f.images.host_occupied() == 3,
            "failed preparation can recover and complete on the same sources");
}

void unavailable_and_fragmented(DeviceContext& device) {
    Fixture f(device.device(), 3);
    Store no_host(f.physical, nullptr, 3);
    const auto source = no_host.reserve_logical_destination();
    require(source.has_value(), "logical source without Host");
    // Empty preparation needs neither storage nor copies.
    require(no_host.prepare_host_export({}, device.transfer_view()).kind == HostKind::Prepared,
            "empty set requires no Host arena");
    require(no_host.release(*source), "release unused logical reservation");

    // Two holes together fit one image, but neither hole is a contiguous image.
    const auto unit = HostContextArena::alignment;
    require(f.schema.image_stride >= 2 * unit, "fixture image spans multiple allocation units");
    HostContextArena fragmented(2 * f.schema.image_stride, unit);
    auto left = fragmented.allocate(f.schema.image_stride - unit);
    auto barrier = fragmented.allocate(unit);
    auto right = fragmented.allocate(f.schema.image_stride - unit);
    auto end = fragmented.allocate(unit);
    require(left && barrier && right && end, "fragmentation allocations");
    left.reset();
    right.reset();
    // Use a separate physical store so logical source slots have one owner.
    StateImageStorage physical({f.backing.data(), f.backing.size(), device.device()}, f.schema);
    Store images(physical, &fragmented, 8);
    const auto image = images.reserve_destination();
    require(image.has_value(), "fragmentation source");
    images.complete_initialization(*image);
    images.freeze(*image);
    images.retain_checkpoint_reference(*image);
    const auto result = images.prepare_host_export({&*image, 1}, device.transfer_view());
    require(result.kind == HostKind::HostContiguity && result.shortage_bytes == 0 &&
                images.device_resident(*image) && fragmented.reserved_bytes() == 2 * unit,
            "contiguity is not a destructive byte deficit");
    // E89: a contiguity-only miss also prepares through the Device route, reserving nothing.
    const auto staged = staged_after_refusal(images, std::span<const Store::Handle>(&*image, 1));
    require(staged.kind == HostKind::Prepared && staged_images(staged) == 1 &&
                fragmented.reserved_bytes() == 2 * unit && images.device_resident(*image) &&
                !images.host_resident(*image),
            "Host contiguity miss prepares a Device-staged export without Host reservation");
    {
        const auto reader = images.read(*image);
        require(reader.device_slot() == images.physical_slot(*image) && images.source_pins(*image) == 1,
                "contiguity-staged reader pins the Device replica");
    }
    require(images.source_pins(*image) == 0, "contiguity-staged reader released its pin");

    StateImageStorage absent_physical({f.backing.data(), f.backing.size(), device.device()}, f.schema);
    Store absent(absent_physical, nullptr, 3);
    const auto missing = absent.reserve_destination();
    require(missing.has_value(), "no-arena source");
    absent.complete_initialization(*missing);
    absent.freeze(*missing);
    const auto unavailable = absent.prepare_host_export({&*missing, 1}, device.transfer_view());
    require(unavailable.kind == HostKind::NoHostArena && unavailable.shortage_bytes == 0,
            "no arena is not contended capacity");
}

// Serializes one exported image as ExportLease::write does for a State record: a zeroed
// record, then each field's layer rows at its image offset. Device sources use the D2H
// pitched copy and Host sources use memcpy rows, exactly as the record's copy flag selects.
std::vector<std::byte> serialize(const StateImageLayout& schema, ConstMemorySpan source) {
    std::vector<std::byte> record(schema.image_stride);
    const bool device = source.device != Device::cpu();
    for (const auto& field : schema.fields) {
        const auto* from = static_cast<const std::byte*>(source.data) + field.tensor.region.offset;
        auto* to = record.data() + field.tensor.region.offset;
        if (device) {
            CUDA_CHECK(cudaMemcpy2DAsync(to, field.layer_stride, from, field.layer_stride,
                                         field.tensor.region.bytes, field.layers,
                                         cudaMemcpyDeviceToHost, nullptr));
        } else {
            for (std::uint32_t layer = 0; layer < field.layers; ++layer)
                std::memcpy(to + layer * field.layer_stride, from + layer * field.layer_stride,
                            field.tensor.region.bytes);
        }
    }
    return record;
}

// The payload a source filled with `value` must produce: field bytes, every hole zero.
std::vector<std::byte> expected_payload(const StateImageLayout& schema, std::byte value) {
    std::vector<std::byte> record(schema.image_stride);
    for (const auto& field : schema.fields)
        for (std::uint32_t layer = 0; layer < field.layers; ++layer)
            std::fill_n(record.begin() + static_cast<std::ptrdiff_t>(field.tensor.region.offset +
                                                                       layer * field.layer_stride),
                        field.tensor.region.bytes, value);
    return record;
}

// E89: a mixed export set (one image with both replicas, two Device-only) serializes the same
// bytes through the Device-staged route as through the Host route, with zero-filled holes.
void staged_payload_identity(DeviceContext& device) {
    using Residency = models::context::StateReplicaResidency;
    Fixture f(device.device(), 3);
    auto backup = f.images.reserve_device_to_host(f.sources[0]);
    require(backup.has_value(), "mixed-set Host backup");
    backup->enqueue(device.transfer_view());
    CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
    backup->publish(true);
    auto hog = f.host.allocate(2 * f.schema.image_stride);
    require(hog.has_value(), "the rest of the Host arena is held");
    const auto host_copies = copies;
    const auto device_rows = rows_2d;
    require(f.images.prepare_host_export(f.sources, device.transfer_view()).kind == HostKind::HostCapacity &&
                copies == host_copies, "held arena refuses the Host route without copies");
    const auto staged = f.images.prepare_device_export(f.sources);
    require(staged.kind == HostKind::Prepared && staged.device_images == 2 && copies == host_copies &&
                f.images.residency(f.sources[0]) == Residency::HostOnly &&
                f.images.residency(f.sources[1]) == Residency::DeviceOnly &&
                f.images.residency(f.sources[2]) == Residency::DeviceOnly,
            "mixed set: the Both image releases its Device replica, Device-only images stay");
    std::vector<std::vector<std::byte>> via_device;
    {
        std::vector<Store::ReadLease> readers;
        for (const auto source : f.sources) readers.push_back(f.images.read(source));
        for (const auto& reader : readers) via_device.push_back(serialize(f.schema, reader.export_view()));
    }
    require(rows_2d == device_rows + 2 * f.schema.fields.size(),
            "only the two Device-only images were read with Device rows");
    hog.reset();
    const auto room_copies = copies;
    const auto prepared = f.images.prepare_host_export(f.sources, device.transfer_view());
    require(prepared.kind == HostKind::Prepared && prepared.device_images == 0 &&
                copies == room_copies + 2 && f.images.device_occupied() == 0 && f.images.host_occupied() == 3,
            "once Host room exists the Host route copies exactly the two missing images");
    const auto host_rows = rows_2d;
    for (std::size_t i = 0; i < f.sources.size(); ++i) {
        const auto reader = f.images.read(f.sources[i]);
        const auto via_host = serialize(f.schema, reader.export_view());
        require(via_host == via_device[i], "Host and Device-staged routes serialized different payload bytes");
        require(via_host == expected_payload(f.schema, std::byte(i + 1)),
                "payload is not exactly the field bytes with zero-filled holes");
    }
    require(rows_2d == host_rows, "Host route payload used Device rows");
}

// E89 review condition (a): with exactly enough Host room the Host route takes every image;
// one image short is the only way to the Device-staged route.
void host_route_first(DeviceContext& device) {
    Fixture exact(device.device(), 3);
    const auto before = copies;
    const auto prepared = exact.images.prepare_host_export(exact.sources, device.transfer_view());
    require(prepared.kind == HostKind::Prepared && prepared.device_images == 0 && copies == before + 3 &&
                exact.images.device_occupied() == 0 && exact.images.host_occupied() == 3,
            "exact Host room took the Host route");
    Fixture short_one(device.device(), 2);
    const auto refused = short_one.images.prepare_host_export(short_one.sources, device.transfer_view());
    require(refused.kind == HostKind::HostCapacity && refused.shortage_bytes == short_one.schema.image_stride,
            "one image short refuses the Host route with its exact deficit");
    require(short_one.images.prepare_device_export(short_one.sources).device_images == 3,
            "only the refused set is Device-staged");
}

// E89 review condition (b): a second export never shares a pinned Device image; it is Busy
// until the first export's readers retire, then prepares.
void staged_reader_busy_then_retry(DeviceContext& device) {
    Fixture f(device.device(), 0);
    require(f.images.prepare_device_export(f.sources).kind == HostKind::Prepared, "first staged export");
    std::vector<Store::ReadLease> first;
    for (const auto source : f.sources) first.push_back(f.images.read(source));
    require(f.images.prepare_device_export(f.sources).kind == HostKind::Busy &&
                f.images.prepare_host_export(f.sources, device.transfer_view()).kind == HostKind::Busy,
            "a second export met pinned Device images without Busy");
    first.clear();
    require(f.images.prepare_device_export(f.sources).kind == HostKind::Prepared,
            "the second export did not prepare after the first retired");
    f.unchanged_device_sources();
}

// E89 (plan correction 2): while a staged reader pins a Device-only image, the predicate every
// native demotion and release plan applies excludes it, and replica drops refuse; nothing throws.
void staged_pin_blocks_demotion(DeviceContext& device) {
    Fixture f(device.device(), 0);
    require(f.images.prepare_device_export(f.sources).kind == HostKind::Prepared, "staged export");
    {
        const auto reader = f.images.read(f.sources[0]);
        require(!f.images.can_release_after_checkpoint_references(f.sources[0], 1) &&
                    !f.images.can_release(f.sources[0]) && !f.images.drop_device_replica(f.sources[0]) &&
                    !f.images.drop_host_replica(f.sources[0]) && f.images.device_resident(f.sources[0]),
                "a pinned Device-only image was offered to demotion or release");
        require(f.images.can_release_after_checkpoint_references(f.sources[1], 1),
                "an unpinned image lost its demotion eligibility");
    }
    require(f.images.can_release_after_checkpoint_references(f.sources[0], 1),
            "the retired reader still blocks demotion");
    f.unchanged_device_sources();
}
} // namespace

int main() {
    struct Case {
        const char* name;
        void (*run)(ninfer::DeviceContext&);
    };
    constexpr std::array cases{
        Case{"export_leased_fork", export_leased_fork},
        Case{"existing_host_replica", existing_host_replica},
        Case{"failure_recovery", failure_recovery},
        Case{"unavailable_and_fragmented", unavailable_and_fragmented},
        Case{"staged_payload_identity", staged_payload_identity},
        Case{"host_route_first", host_route_first},
        Case{"staged_reader_busy_then_retry", staged_reader_busy_then_retry},
        Case{"staged_pin_blocks_demotion", staged_pin_blocks_demotion},
    };
    try {
        ninfer::DeviceContext device;
        int failures = 0;
        // Every case runs, so a red run names each failing case.
        for (const auto& test : cases) {
            try {
                test.run(device);
                std::cout << "PASS " << test.name << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            }
        }
        if (failures) return 1;
        std::cout << "PASS export State readers release execution slots without changing bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
