#pragma once

#include "core/buffer.h"
#include "ninfer/ops/gdn_control.h"
#include "ops/op_tester.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::test::gdn_control {

inline constexpr float kEps = 1.0e-6F;

enum class Storage { Parent, Pair };
enum class Pattern {
    Random,
    ZeroInput,
    ZeroA,
    ZeroB,
    Sparse,
    Cancellation,
    GateEdges,
    TinyNormInput,
    LargeNormInput,
    ZeroNormGain
};
enum class Execution { Eager, Graph };

struct Profile {
    int heads, input_rows;
    Storage storage = Storage::Parent;
};

inline constexpr std::array kProfiles{Profile{48, 5120}, Profile{48, 5120, Storage::Pair},
                                      Profile{32, 2048}, Profile{48, 2560}};

struct Case {
    int columns;
    Pattern pattern     = Pattern::Random;
    Execution execution = Execution::Eager;
    bool prefix         = false;
};

std::string label(Profile profile, Case test_case, bool normalize_input);
std::vector<std::uint16_t> bits(const std::vector<float>& values);

class Weights {
public:
    explicit Weights(Profile profile, Pattern pattern);
    void upload();
    int check(std::string_view name) const;
    Profile profile;
    std::vector<float> a, b;
    DeviceBuffer first;
    DeviceBuffer second;
    ops::ProjectionWeights prepared;
private:
    std::vector<std::uint16_t> first_bits_, second_bits_;
};

struct Fixture {
    Fixture(Profile profile, Case test_case, bool normalize_input);
    void upload();
    void advance();
    void repaint(cudaStream_t stream);
    void invoke(MemoryArena& arena, CudaExecutionView execution);
    int check(std::string_view name) const;
    Profile profile;
    Case test_case;
    bool normalize_input;
    Weights weights;
    std::size_t capacity, prefix;
    std::vector<float> x, norm_gain, a_log, dt_bias;
    DeviceBuffer device_x;
    DeviceBuffer device_norm_weight;
    DeviceBuffer device_a_log;
    DeviceBuffer device_dt_bias;
    GuardedDeviceBuffer g, beta, normalized, scratch;
    Tensor input, norm_weight, a_log_tensor, dt_bias_tensor, normalized_tensor;
    ops::GdnControlOutputs outputs;
    MemoryArena arena;
};

std::size_t workspace_capacity(const ops::ProjectionWeights& weights, bool normalize_input,
                               int first, int last);

} // namespace ninfer::test::gdn_control
