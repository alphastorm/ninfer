// Exact oracle for the SHA-256 codec that binds every checkpoint payload digest and the
// manifest origin MAC. Two independent references: the FIPS 180-4 known answers, which the
// bulk path (SHA extensions where the CPU has them, the scalar schedule elsewhere) must
// reproduce bit for bit, and the scalar schedule itself, reached by feeding the hasher in
// sub-block updates so the bulk dispatch never runs, cross-checked against bulk updates over
// the same bytes at every length that straddles a block boundary.
#include "core/sha256.h"
#include "core/sha256_x86.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
    }
}

std::string hex_of(std::span<const std::byte> bytes) {
    return ninfer::crypto::sha256_hex(ninfer::crypto::sha256(bytes));
}

// Never reaches the bulk dispatch: every update is shorter than one block.
ninfer::crypto::Sha256Digest scalar_reference(std::span<const std::byte> bytes) {
    ninfer::crypto::Sha256 hasher;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 63) {
        hasher.update(bytes.subspan(offset, std::min<std::size_t>(63, bytes.size() - offset)));
    }
    return hasher.finish();
}

void test_fips_180_4_known_answers() {
    struct Vector {
        std::string_view message;
        std::string_view digest;
    };
    const Vector vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqr"
         "lmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    for (const Vector& vector : vectors) {
        const auto bytes = std::as_bytes(std::span(vector.message.data(), vector.message.size()));
        require(hex_of(bytes) == vector.digest,
                "FIPS 180-4 vector of " + std::to_string(vector.message.size()) + " bytes");
    }
    // One million 'a': 15,625 whole blocks through the bulk path.
    const std::vector<std::byte> million(1'000'000, std::byte{'a'});
    require(hex_of(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "FIPS 180-4 one-million-a vector");
}

void test_bulk_matches_scalar_at_block_boundaries() {
    std::mt19937_64 rng(0x5eed);
    std::vector<std::byte> bytes(4096);
    for (std::byte& value : bytes) { value = static_cast<std::byte>(rng()); }
    const std::size_t lengths[] = {0,  1,  55,  56,  57,  63,  64,  65,  119, 127,
                                   128, 129, 191, 192, 193, 255, 256, 1023, 1024, 4096};
    for (const std::size_t length : lengths) {
        const auto slice = std::span<const std::byte>(bytes).first(length);
        require(ninfer::crypto::sha256(slice) == scalar_reference(slice),
                "bulk and scalar digests differ at " + std::to_string(length) + " bytes");
    }
}

void test_ragged_updates_match_one_shot() {
    std::mt19937_64 rng(42);
    std::vector<std::byte> bytes(8U << 20);
    for (std::byte& value : bytes) { value = static_cast<std::byte>(rng()); }
    ninfer::crypto::Sha256 ragged;
    std::uniform_int_distribution<std::size_t> step(1, 300'000);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t count = std::min(step(rng), bytes.size() - offset);
        ragged.update(std::span<const std::byte>(bytes).subspan(offset, count));
        offset += count;
    }
    require(ragged.finish() == ninfer::crypto::sha256(bytes),
            "ragged multi-megabyte updates differ from the one-shot digest");
    require(ninfer::crypto::sha256(bytes) == scalar_reference(bytes),
            "multi-megabyte bulk digest differs from the scalar schedule");
}

void test_hmac_rfc4231() {
    // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?".
    const std::string_view key  = "Jefe";
    const std::string_view data = "what do ya want for nothing?";
    const auto mac = ninfer::crypto::hmac_sha256(std::as_bytes(std::span(key.data(), key.size())),
                                                 std::as_bytes(std::span(data.data(), data.size())));
    require(ninfer::crypto::sha256_hex(mac) ==
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            "RFC 4231 HMAC-SHA256 test case 2");
}

} // namespace

int main() {
    std::printf("sha extensions: %s\n", ninfer::crypto::x86::sha256_available() ? "yes" : "no");
    test_fips_180_4_known_answers();
    test_bulk_matches_scalar_at_block_boundaries();
    test_ragged_updates_match_one_shot();
    test_hmac_rfc4231();
    if (failures != 0) { return 1; }
    std::printf("ok\n");
    return 0;
}
