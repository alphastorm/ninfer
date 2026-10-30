#include "core/paged_kv_storage.h"
#include "ops/kv_cache/kernels/rk4v4.cuh"
#include "ops/rk4v4_reference.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace ninfer;
namespace codec = ninfer::ops::detail::kv;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
unsigned bits(__half value) { std::uint16_t b; std::memcpy(&b, &value, 2); return b; }

int main() {
    try {
        // Generated independently using Python binary64 arithmetic + struct.pack('<e').
        // Input x[d] = ((17*d)%31-15)/8; the oracle applies H256/16 to K only.
        constexpr std::array<std::uint8_t, 128> golden{
            0x98,0x08,0x08,0x00,0x12,0x19,0x11,0x99,0x91,0x99,0x9b,0x19,0xaa,0x00,0x02,0x0a,
            0x09,0xa0,0xc0,0x00,0xf1,0x31,0x51,0x39,0x39,0x99,0xb9,0x91,0xea,0xc0,0xf0,0x20,
            0x08,0x47,0x00,0x00,0x2a,0x2a,0xaa,0x2e,0xaa,0xaa,0x2a,0xa2,0x00,0xc4,0x0c,0x0c,
            0xc0,0xf4,0x00,0x00,0x62,0x62,0x6a,0x22,0xaa,0xaa,0xa2,0x2a,0xf0,0xcc,0x40,0x40,
            0x08,0x00,0x02,0xa0,0x11,0x99,0x9c,0x91,0x9c,0x19,0x1f,0x16,0x02,0x0a,0x0d,0x2a,
            0xd0,0x00,0x20,0x0a,0x61,0x49,0x79,0xc9,0xc9,0x91,0x11,0xc1,0xf0,0x20,0x00,0x22,
            0x00,0x00,0xc0,0x0c,0xaa,0x2d,0xa2,0xa2,0x2a,0xa2,0x27,0x27,0x0c,0x0c,0x4c,0x00,
            0x00,0x00,0x0c,0xc0,0x5a,0x22,0xda,0xda,0xa2,0x2a,0xd2,0xd2,0x40,0x40,0x44,0x60};
        constexpr unsigned scales[] = {0x2903, 0x23fb, 0x276e, 0x2493};
        std::array<double, 256> input{};
        for (int d = 0; d < 256; ++d) input[d] = ((17*d)%31-15)/8.0;
        const auto oracle = rk4_reference::encode(input, true);
        require(oracle.codes == golden, "independent FP64 key golden");
        auto rotated = input; rk4_reference::rotate(rotated);
        for (int g = 0; g < 4; ++g) {
            require(oracle.scales[g] == scales[g], "independent FP64 scale golden");
            double sum = 0; for (int i = 0; i < 64; ++i) sum += rotated[g*64+i]*rotated[g*64+i];
            const float inv = 1.0f / static_cast<float>(std::sqrt(sum / 64));
            float xc = 0, cc = 0;
            for (int i = 0; i < 64; ++i) {
                const int d = g*64+i, index = codec::rk4_key_index(static_cast<float>(rotated[d]), inv);
                require(index == ((golden[d/2] >> (4*(d&1))) & 15), "production key index golden");
                const int c = codec::rk4_key_code(index);
                xc += static_cast<float>(rotated[d])*c; cc += c*c;
                require(c * rk4_reference::half(scales[g]) == oracle.decoded[d], "key decode round trip");
            }
            require(bits(codec::rk4_key_scale(xc, cc)) == scales[g], "production fitted scale golden");
        }
        for (const double magnitude : {0.0, 0x1p-20, 0.125, 1.0, 31.0}) {
            std::array<double, 256> values{};
            for (int d = 0; d < 256; ++d) values[d] = ((d%15)-7)*magnitude;
            const auto expected = rk4_reference::encode(values, false);
            const auto scale = codec::rk4_value_scale(static_cast<float>(7*magnitude));
            require(bits(scale) == expected.scales[0], "value scale golden");
            for (int d = 0; d < 256; d += 2) {
                const auto byte = codec::rk4_pack(codec::rk4_value_index(values[d], scale), codec::rk4_value_index(values[d+1], scale));
                require(byte == expected.codes[d/2], "value encode golden");
                require(codec::rk4_value_code(byte) * __half2float(scale) == expected.decoded[d], "value low round trip");
                require(codec::rk4_value_code(byte >> 4) * __half2float(scale) == expected.decoded[d+1], "value high round trip");
            }
        }
        constexpr int codebook[] = {6,18,31,44,58,75,96,127,-6,-18,-31,-44,-58,-75,-96,-127};
        for (int i = 0; i < 16; ++i) require(codec::rk4_key_code(i) == codebook[i], "signed codebook");
        const auto packed = paged_kv_storage_layout(KvCacheStorage::Rk4V4, 256);
        require(packed.key.physical_bytes() == 136 && packed.value.physical_bytes() == 144, "packed row sizes");
        require(packed.physical_bytes_per_token_head()*64 == 17920, "27B packed bytes/token");
        require(packed.physical_bytes_per_token_head()*64*262144 == 4697620480ULL, "262K packed capacity");
        require(paged_kv_storage_layout(KvCacheStorage::Int8Group64,256).physical_bytes_per_token_head()*64 == 33792, "unchanged INT8 capacity");
        require(paged_kv_storage_layout(KvCacheStorage::BFloat16,256).physical_bytes_per_token_head()*64 == 65536, "unchanged BF16 capacity");
        bool refused = false;
        try { (void)paged_kv_storage_layout(KvCacheStorage::Rk4V4,128); } catch (const std::invalid_argument&) { refused = true; }
        require(refused, "D128 must not silently select D256 codec");
        std::cout << "rk4v4 independent FP64 golden, round-trip, and capacity contracts passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
