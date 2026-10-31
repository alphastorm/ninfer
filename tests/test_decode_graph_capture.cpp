#include "core/decode_graph.h"
#include "cuda_interpose.h"

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
unsigned char stream_token, graph_token;
const auto stream = reinterpret_cast<cudaStream_t>(&stream_token);
const auto graph = reinterpret_cast<cudaGraph_t>(&graph_token);
int begins = 0, ends = 0, destroys = 0, bodies = 0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

// Exercise the real capture owner on GPU-free CI, not a substitute graph wrapper.
// This checks stream admission/lifetime; numerical kernels still require the GPU suite.
#define NINFER_CAPTURE_TEST_CUDA(name) NINFER_TEST_CUDA_WRAP(name)
extern "C" {
cudaError_t CUDARTAPI NINFER_CAPTURE_TEST_CUDA(cudaStreamBeginCapture)(
    cudaStream_t selected, cudaStreamCaptureMode mode) {
    ++begins;
    require(selected == stream, "capture did not use the selected non-legacy stream");
    require(mode == cudaStreamCaptureModeThreadLocal, "capture mode changed");
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_CAPTURE_TEST_CUDA(cudaStreamEndCapture)(
    cudaStream_t selected, cudaGraph_t* result) {
    ++ends;
    require(selected == stream, "capture end changed streams");
    *result = graph;
    return cudaSuccess;
}
cudaError_t CUDARTAPI NINFER_CAPTURE_TEST_CUDA(cudaGraphDestroy)(cudaGraph_t selected) {
    ++destroys;
    require(selected == graph, "destroyed an unowned graph");
    return cudaSuccess;
}
}

int main() {
    try {
        for (const auto legacy : {static_cast<cudaStream_t>(nullptr), cudaStreamLegacy}) {
            ninfer::DecodeGraphDefinition definition;
            bool refused = false;
            try {
                definition.capture(legacy, [] { ++bodies; });
            } catch (const std::invalid_argument& error) {
                refused = std::string_view(error.what()).find("non-legacy") != std::string_view::npos;
            }
            require(refused, "legacy capture was not rejected at the host boundary");
            require(begins == 0 && ends == 0 && bodies == 0,
                    "legacy rejection reached CUDA or the capture body");
            require(!definition.ready(), "rejected capture published a graph");
        }
        {
            ninfer::DecodeGraphDefinition definition;
            definition.capture(stream, [] { ++bodies; });
            require(definition.ready() && begins == 1 && ends == 1 && bodies == 1,
                    "valid stream did not capture exactly one body");
        }
        require(destroys == 1, "completed graph did not release ownership");
        {
            ninfer::DecodeGraphDefinition definition;
            try {
                definition.capture(stream, [] { throw std::runtime_error("body failure"); });
            } catch (const std::runtime_error& error) {
                require(std::string_view(error.what()) == "body failure", "unexpected failure");
            }
            require(!definition.ready() && begins == 2 && ends == 2 && destroys == 2,
                    "failed body left capture active or published a graph");
            definition.capture(stream, [] { ++bodies; });
            require(definition.ready() && begins == 3 && ends == 3 && bodies == 2,
                    "capture did not recover after body failure");
        }
        require(destroys == 3, "recovered graph did not release ownership");
        std::cout << "decode graph non-legacy stream admission and capture lifetime passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
