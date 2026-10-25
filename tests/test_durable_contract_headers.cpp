#include "runtime/contract/durable_checkpoint_set.h"
#include "runtime/contract/durable_engine_api.h"
#include "runtime/contract/runtime_fingerprint.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace ninfer::runtime;

static_assert(kDurableSessionSchemaVersion == 3);
static_assert(kRuntimeFingerprintVersion == 2);
static_assert(kDurableCheckpointSetFormat == "ninfer-durable-checkpoint-set/1");
static_assert(!std::is_copy_constructible_v<DurableEngineImport>);
static_assert(std::is_nothrow_move_constructible_v<DurableEngineImport>);
static_assert(std::is_nothrow_move_assignable_v<DurableEngineImport>);
static_assert(std::is_same_v<DurableLocalId, std::uint32_t>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::checkpoint_layout),
                             DurableNativeLayout (*)(const ninfer::Engine&)>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::frontend_identity),
                             DurableFrontendIdentity (*)(const ninfer::Engine&)>);

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected durable fixture directory");
        for (const char* name : {"manifest-text.json", "manifest-dflash2.json", "ack-posix.json", "ack-windows.json"}) {
            std::ifstream input(std::string(argv[1]) + "/" + name, std::ios::binary);
            if (!input) throw std::runtime_error("cannot open canonical fixture");
            const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            const auto value = nlohmann::json::parse(bytes);
            if (value.dump() != bytes) throw std::runtime_error("production JSON encoder differs from canonical fixture");
        }
        const nlohmann::json value = {
            {"z", {true, false, nullptr, std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::uint64_t>::max()}},
            {"a", std::string("\xc3\xa9/\"\\\b\t\n\f\r\0\x1f", 12)}
        };
        const std::string expected = "{\"a\":\"\xc3\xa9/\\\"\\\\\\b\\t\\n\\f\\r\\u0000\\u001f\",\"z\":[true,false,null,-9223372036854775808,18446744073709551615]}";
        if (value.dump() != expected) throw std::runtime_error("production JSON encoder differs on Unicode/control/integer vector");
        std::cout << "durable headers and 5 canonical encoder vectors passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
