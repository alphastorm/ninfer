#include "core/sha256_x86.h"

#if defined(__x86_64__) || defined(_M_X64)
#    include <immintrin.h>
#    if defined(_MSC_VER)
#        include <intrin.h>
#        define NINFER_SHA_TARGET
#    else
#        include <cpuid.h>
#        define NINFER_SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#    endif
#endif

namespace ninfer::crypto::x86 {

#if defined(__x86_64__) || defined(_M_X64)

namespace {

bool probe_sha256() noexcept {
    std::uint32_t leaf1_ecx = 0;
    std::uint32_t leaf7_ebx = 0;
#    if defined(_MSC_VER)
    int leaf[4]{};
    __cpuid(leaf, 0);
    if (static_cast<std::uint32_t>(leaf[0]) < 7U) { return false; }
    __cpuid(leaf, 1);
    leaf1_ecx = static_cast<std::uint32_t>(leaf[2]);
    __cpuidex(leaf, 7, 0);
    leaf7_ebx = static_cast<std::uint32_t>(leaf[1]);
#    else
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0 || eax < 7U) { return false; }
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) { return false; }
    leaf1_ecx = ecx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) { return false; }
    leaf7_ebx = ebx;
#    endif
    constexpr std::uint32_t kSsse3  = 1U << 9U;
    constexpr std::uint32_t kSse41  = 1U << 19U;
    constexpr std::uint32_t kShaExt = 1U << 29U;
    return (leaf1_ecx & kSsse3) != 0U && (leaf1_ecx & kSse41) != 0U &&
           (leaf7_ebx & kShaExt) != 0U;
}

constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

// Two SHA256RNDS2 steps consume one 4-word message slice: the low pair of round constants, then
// the high pair after the slice is rotated down.
template <std::size_t Group>
NINFER_SHA_TARGET inline void four_rounds(__m128i& state0, __m128i& state1,
                                          __m128i message) noexcept {
    __m128i keyed = _mm_add_epi32(
        message, _mm_loadu_si128(reinterpret_cast<const __m128i*>(kRound.data() + 4U * Group)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, keyed);
    keyed  = _mm_shuffle_epi32(keyed, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, keyed);
}

// Runs group G (rounds 4G..4G+3) on the completed slice w[G % 4] and advances the schedule:
// the following slice is completed from this slice's tail (alignr + msg2), and the slice
// before it takes its first expansion step (msg1). Groups 13-15 no longer expand.
template <std::size_t Group>
NINFER_SHA_TARGET inline void schedule_group(__m128i& state0, __m128i& state1,
                                             __m128i (&w)[4]) noexcept {
    __m128i& current  = w[Group % 4];
    __m128i& previous = w[(Group + 3) % 4];
    __m128i& next     = w[(Group + 1) % 4];
    four_rounds<Group>(state0, state1, current);
    if constexpr (Group <= 14) {
        next = _mm_add_epi32(next, _mm_alignr_epi8(current, previous, 4));
        next = _mm_sha256msg2_epu32(next, current);
    }
    if constexpr (Group <= 12) { previous = _mm_sha256msg1_epu32(previous, current); }
}

} // namespace

bool sha256_available() noexcept {
    static const bool available = probe_sha256();
    return available;
}

NINFER_SHA_TARGET void sha256_process_blocks(std::array<std::uint32_t, 8>& state,
                                             const std::byte* data, std::size_t blocks) noexcept {
    const __m128i byte_swap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);

    // The instructions hold the working state as ABEF / CDGH pairs.
    __m128i tmp    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data()));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data() + 4));
    tmp            = _mm_shuffle_epi32(tmp, 0xB1);
    state1         = _mm_shuffle_epi32(state1, 0x1B);
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);
    state1         = _mm_blend_epi16(state1, tmp, 0xF0);

    for (; blocks != 0; --blocks, data += 64) {
        const __m128i saved0 = state0;
        const __m128i saved1 = state1;
        __m128i w[4];
        w[0] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data)),
                                byte_swap);
        four_rounds<0>(state0, state1, w[0]);
        w[1] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16)),
                                byte_swap);
        four_rounds<1>(state0, state1, w[1]);
        w[0] = _mm_sha256msg1_epu32(w[0], w[1]);
        w[2] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 32)),
                                byte_swap);
        four_rounds<2>(state0, state1, w[2]);
        w[1] = _mm_sha256msg1_epu32(w[1], w[2]);
        w[3] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 48)),
                                byte_swap);
        schedule_group<3>(state0, state1, w);
        schedule_group<4>(state0, state1, w);
        schedule_group<5>(state0, state1, w);
        schedule_group<6>(state0, state1, w);
        schedule_group<7>(state0, state1, w);
        schedule_group<8>(state0, state1, w);
        schedule_group<9>(state0, state1, w);
        schedule_group<10>(state0, state1, w);
        schedule_group<11>(state0, state1, w);
        schedule_group<12>(state0, state1, w);
        schedule_group<13>(state0, state1, w);
        schedule_group<14>(state0, state1, w);
        schedule_group<15>(state0, state1, w);
        state0 = _mm_add_epi32(state0, saved0);
        state1 = _mm_add_epi32(state1, saved1);
    }

    tmp    = _mm_shuffle_epi32(state0, 0x1B);
    state1 = _mm_shuffle_epi32(state1, 0xB1);
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);
    state1 = _mm_alignr_epi8(state1, tmp, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state.data()), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state.data() + 4), state1);
}

#else

bool sha256_available() noexcept { return false; }

void sha256_process_blocks(std::array<std::uint32_t, 8>&, const std::byte*, std::size_t) noexcept {}

#endif

} // namespace ninfer::crypto::x86
