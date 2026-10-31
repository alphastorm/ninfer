// GPU-free execution of the real StateImageStore. Only CUDA allocation/copy/stream entry
// points are interposed; ownership, reservations, epochs and replica publication are native.
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
std::size_t copies = 0, synchronizations = 0, fail_copy = 0;
}

// Expand CUDA's versioned API aliases before the Linux linker-wrap token is formed.
#define NINFER_EXPORT_STATE_CUDA(name) NINFER_TEST_CUDA_WRAP(name)

extern "C" {
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaGetDeviceCount)(int* count) {
    std::cerr << "cudaGetDeviceCount\n";
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
    std::cerr << "cudaGetDeviceProperties\n";
    *props = {};
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaStreamCreateWithFlags)(cudaStream_t* stream,
                                                                                unsigned) {
    std::cerr << "cudaStreamCreateWithFlags\n";
    static std::array<unsigned char, 2> streams{};
    static std::size_t created = 0;
    *stream = reinterpret_cast<cudaStream_t>(&streams[created++ % streams.size()]);
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaStreamDestroy)(cudaStream_t) { return cudaSuccess; }
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaMallocHost)(void** memory, std::size_t bytes) {
    std::cerr << "cudaMallocHost " << bytes << '\n';
    *memory = ::operator new(bytes, std::align_val_t{256}, std::nothrow);
    return *memory ? cudaSuccess : cudaErrorMemoryAllocation;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaFreeHost)(void* memory) {
    std::cerr << "cudaFreeHost\n";
    ::operator delete(memory, std::align_val_t{256});
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaMemcpyAsync)(void* to, const void* from,
    std::size_t bytes, cudaMemcpyKind, cudaStream_t) {
    std::cerr << "cudaMemcpyAsync " << copies + 1 << " fail=" << fail_copy << '\n';
    if (++copies == fail_copy) return cudaErrorInvalidValue;
    std::memcpy(to, from, bytes);
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_EXPORT_STATE_CUDA(cudaStreamSynchronize)(cudaStream_t) {
    std::cerr << "cudaStreamSynchronize\n";
    ++synchronizations;
    return cudaSuccess;
}
}

namespace {
using namespace ninfer;
using Store = models::context::StateImageStore;

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
};

void export_leased_fork(DeviceContext& device) {
    Fixture f(device.device(), 3);
    std::vector<Store::ReadLease> exports;
    for (auto source : f.sources) exports.push_back(f.images.read(source));
    // Reproduce the old export sequence: three available slots are all retained for disk I/O.
    require(!f.images.reserve_destination(), "old Device export readers reproduce fork starvation");
    require(!f.images.drop_device_replica(f.sources[0]), "reader ownership guard remains enforced");
    exports.clear();

    const auto before = copies;
    require(f.images.prepare_host_export(f.sources, device.transfer_view()), "prepare Host export");
    require(copies == before + 3 && f.images.device_occupied() == 0 &&
                f.images.host_occupied() == 3, "one copy per missing Host replica frees all slots");
    for (std::size_t i = 0; i < f.sources.size(); ++i) {
        exports.push_back(f.images.read(f.sources[i]));
        require(exports.back().epoch() == f.epochs[i], "export retains the original content epoch");
    }
    require(f.images.prepare_host_export(f.sources, device.transfer_view()) && copies == before + 3,
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
    require(f.images.prepare_host_export(f.sources, device.transfer_view()) &&
                copies == before + 2 && f.images.device_occupied() == 0,
            "existing Host replica is reused and its Device replica is released");
}

void failure_recovery(DeviceContext& device) {
    for (const auto slots : {0U, 2U}) {
        Fixture f(device.device(), slots);
        const auto before = copies;
        require(!f.images.prepare_host_export(f.sources, device.transfer_view()) && copies == before,
                "all Host capacity is reserved before any copy");
        f.unchanged_device_sources();
    }
    Fixture f(device.device(), 3);
    {
        auto reader = f.images.read(f.sources[1]);
        const auto before = copies;
        require(!f.images.prepare_host_export(f.sources, device.transfer_view()) && copies == before,
                "an existing Device reader is never invalidated to prepare export");
    }
    f.unchanged_device_sources();
    const auto before_sync = synchronizations;
    fail_copy = copies + 2;
    bool failed = false;
    try {
        (void)f.images.prepare_host_export(f.sources, device.transfer_view());
    } catch (const std::runtime_error&) {
        failed = true;
    }
    fail_copy = 0;
    require(failed && synchronizations > before_sync,
            "partial enqueue failure settles submitted copies before reservation rollback");
    f.unchanged_device_sources();
    require(f.images.prepare_host_export(f.sources, device.transfer_view()) &&
                f.images.device_occupied() == 0 && f.images.host_occupied() == 3,
            "failed preparation can recover and complete on the same sources");
}
} // namespace

int main() {
    try {
        std::cerr << "RUN GPU-free DeviceContext\n";
        ninfer::DeviceContext device;
        std::cerr << "RUN export-leased fork\n";
        export_leased_fork(device);
        std::cerr << "RUN existing Host replica\n";
        existing_host_replica(device);
        std::cerr << "RUN failure recovery\n";
        failure_recovery(device);
        std::cout << "PASS export State readers release execution slots without changing bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
