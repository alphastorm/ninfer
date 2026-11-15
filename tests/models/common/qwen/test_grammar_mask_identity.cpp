#include "core/sha256.h"
#include "models/common/qwen/frontend/chat_template.h"
#include "models/common/qwen/frontend/tool_contract.h"
#include "models/common/qwen/frontend/tool_grammar.h"
#include "text/grammar.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace frontend = ninfer::models::qwen::frontend;
using Json = nlohmann::ordered_json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

// Frame words explicitly, so both masks and vocabulary identities are endian-independent.
void hash_word(ninfer::crypto::Sha256& hash, std::uint32_t word) {
    const std::array<std::byte, 4> bytes{
        std::byte(word), std::byte(word >> 8), std::byte(word >> 16), std::byte(word >> 24)};
    hash.update(bytes);
}

std::string mask_digest(std::span<const std::uint32_t> words) {
    ninfer::crypto::Sha256 hash;
    for (auto word : words) hash_word(hash, word);
    return ninfer::crypto::sha256_hex(hash.finish());
}

std::string vocabulary_digest(const std::vector<std::string>& vocabulary) {
    ninfer::crypto::Sha256 hash;
    for (const auto& token : vocabulary) {
        hash_word(hash, static_cast<std::uint32_t>(token.size()));
        hash.update(std::as_bytes(std::span(token.data(), token.size())));
    }
    return ninfer::crypto::sha256_hex(hash.finish());
}

std::vector<std::string> vocabulary() {
    std::vector<std::string> result;
    for (int byte = 0; byte < 256; ++byte) result.emplace_back(1, static_cast<char>(byte));
    const auto add = [&](std::string token) {
        if (!token.empty() && std::find(result.begin(), result.end(), token) == result.end())
            result.push_back(std::move(token));
    };
    const std::array<std::string_view, 20> literals{
        "<tool_call>", "</tool_call>", "<function=", "</function>", "<parameter=",
        "</parameter>", "\n</parameter>\n", frontend::kCanonicalReasoningCloseSerialization,
        "{\"", "\":", "\",\"", "\"}", "\\\"", "\\\\", "true", "false", "null", "[", "]", "\n"};
    for (const auto literal : literals) {
        for (std::size_t cut = 0; cut <= literal.size(); ++cut) {
            const std::string prefix(literal.substr(0, cut));
            const std::string suffix(literal.substr(cut));
            add(prefix);
            add(suffix);
            for (const char cross : {'\'', '\n', ' '}) {
                add(cross + prefix);
                add(suffix + cross);
            }
        }
    }
    result.emplace_back(); // The only special token is EOS, after all ordinary tokens.
    return result;
}

std::vector<std::int32_t> greedy_tokens(const std::vector<std::string>& vocabulary,
                                      std::string_view target) {
    std::vector<std::int32_t> result;
    for (std::size_t offset = 0; offset < target.size();) {
        std::size_t longest = 0;
        std::int32_t selected = -1;
        for (std::size_t id = 0; id < vocabulary.size(); ++id) {
            const auto& token = vocabulary[id];
            if (token.size() > longest && target.substr(offset).starts_with(token)) {
                longest = token.size();
                selected = static_cast<std::int32_t>(id);
            }
        }
        require(selected >= 0, "greedy target is not representable");
        result.push_back(selected);
        offset += longest;
    }
    result.push_back(static_cast<std::int32_t>(vocabulary.size() - 1));
    return result;
}

auto tool_contract(bool required) {
    const auto function = [](std::string_view name, const Json& properties,
                             const Json& required_names) {
        return Json{{"type", "function"},
                    {"function",
                     {{"name", name}, {"strict", true},
                      {"parameters",
                       {{"type", "object"}, {"properties", properties},
                        {"required", required_names}, {"additionalProperties", false}}}}}};
    };
    std::vector<std::string> definitions{
        function("bash", {{"command", {{"type", "string"}}},
                          {"timeout", {{"type", "integer"}}}}, {"command", "timeout"}).dump(),
        function("Edit", {{"file_path", {{"type", "string"}}},
                          {"old_string", {{"type", "string"}}},
                          {"new_string", {{"type", "string"}}},
                          {"replace_all", {{"type", "boolean"}}}},
                 {"file_path", "old_string", "new_string", "replace_all"}).dump()};
    auto custom = function("raw", {{"input", {{"type", "string"}}}}, {"input"});
    custom["function"]["strict"] = false;
    custom["x-ninfer-tool-kind"] = "custom";
    definitions.push_back(custom.dump());
    ninfer::ToolChoice choice;
    choice.mode = required ? ninfer::ToolChoiceMode::Required : ninfer::ToolChoiceMode::Auto;
    return frontend::select_tool_call_contract(
        frontend::build_tool_call_output_contract(definitions), choice);
}

struct Case {
    std::string name;
    std::string target;
    std::shared_ptr<const frontend::ToolCallOutputContract> tools;
    std::optional<ninfer::OutputConstraint> body;
    std::string close;
};

std::vector<Case> cases() {
    const auto body = ninfer::OutputConstraint::json_schema(
        R"({"type":"object","properties":{"ok":{"type":"boolean"},"count":{"type":"integer"},"paths":{"type":"array","items":{"type":"string"}}},"required":["ok","count","paths"],"additionalProperties":false})");
    const std::string answer = R"({"ok":true,"count":3,"paths":["a","b\"\\"]})";
    return {
        {"required_tools",
         "<tool_call>\n<function=bash>\n<parameter=command>\nprintf 'x'\n</parameter>\n"
         "<parameter=timeout>\n30\n</parameter>\n</function>\n</tool_call>\n"
         "<tool_call>\n<function=Edit>\n<parameter=file_path>\na.txt\n</parameter>\n"
         "<parameter=old_string>\nold\n</parameter>\n<parameter=new_string>\nnew\n</parameter>\n"
         "<parameter=replace_all>\nfalse\n</parameter>\n</function>\n</tool_call>\n"
         "<tool_call>\n<function=raw>\n<parameter=input>\nprint('ok')\n"
         "\n</parameter>\n</function>\n</tool_call>",
         tool_contract(true), std::nullopt, {}},
        {"tools_and_json_schema", answer, tool_contract(false), body, {}},
        {"json_schema_reasoning",
         "Check the paths." + std::string(frontend::kCanonicalReasoningCloseSerialization) +
             answer,
         {}, body, std::string(frontend::kCanonicalReasoningCloseSerialization)},
        {"json_schema", answer, {}, body, {}},
    };
}

auto compile(ninfer::text::GrammarCompiler& compiler, const Case& test) {
    if (test.tools)
        return frontend::compile_tool_grammar(compiler, *test.tools, test.close, {}, test.body);
    return compiler.compile(*test.body, test.close, {});
}

Json walk(ninfer::text::GrammarSession& session, const std::vector<std::int32_t>& tokens,
          std::string_view name) {
    const auto words = session.mask_words();
    std::vector<std::uint32_t> single(words), draft_masks(4 * words), restored(words);
    Json result = Json::array();
    for (std::size_t step = 0; step < tokens.size(); ++step) {
        std::array<std::int32_t, 3> drafts{};
        for (std::size_t i = 0; i < drafts.size(); ++i)
            drafts[i] = step + i < tokens.size() ? tokens[step + i] : tokens.back();
        const auto dead = session.masks({}, single);
        const auto draft_dead = session.masks(drafts, draft_masks);
        require(session.masks({}, restored) == dead && restored == single,
                "draft lookahead changed the committed mask");
        require(std::equal(single.begin(), single.end(), draft_masks.begin()),
                "draft lookahead changed its initial mask");
        const auto token = tokens[step];
        const bool allowed = (single[token / 32] & (1u << (token % 32))) != 0;
        result.push_back({{"token", token}, {"masks_sha256", mask_digest(single)},
                          {"draft_masks_sha256", mask_digest(draft_masks)},
                          {"dead", dead}, {"draft_dead", draft_dead}, {"allowed", allowed}});
        if (!allowed) {
            std::cerr << name << " stopped: target token masked at step " << step << '\n';
            break;
        }
        session.accept(token);
        session.confirm();
    }
    return result;
}

Json observe() {
    const auto vocab = vocabulary();
    // Insertion of the next grammar evicts the preceding one. Built observations below
    // prove that the second compilation is not just another matcher on the cached grammar.
    ninfer::text::GrammarCompiler compiler(vocab, {static_cast<std::int32_t>(vocab.size() - 1)}, 1);
    Json rows = Json::array();
    for (const auto& test : cases()) {
        const auto tokens = greedy_tokens(vocab, test.target);
        auto first = compile(compiler, test);
        require(first->observation().cache == ninfer::ConstraintCacheAccess::Built,
                "first grammar compilation unexpectedly hit cache");
        const auto steps = walk(*first, tokens, test.name);
        const auto churn = compiler.compile(ninfer::OutputConstraint::grammar("root ::= \"evict\""),
                                            {}, {});
        auto second = compile(compiler, test);
        require(second->observation().cache == ninfer::ConstraintCacheAccess::Built,
                "grammar was not evicted before the second compilation");
        require(walk(*second, tokens, test.name) == steps,
                "eviction and recompilation changed the mask digests");
        rows.push_back({{"name", test.name}, {"target", test.target},
                        {"terminated", second->observation().terminated}, {"steps", steps}});
    }
    return {{"schema_version", 1}, {"vocabulary_size", vocab.size()},
            {"vocabulary_sha256", vocabulary_digest(vocab)}, {"cases", std::move(rows)}};
}

bool compare(const Json& expected, const Json& actual) {
    if (expected == actual) return true;
    for (const char* field : {"schema_version", "vocabulary_size", "vocabulary_sha256"})
        if (expected.at(field) != actual.at(field))
            std::cerr << field << ": expected " << expected.at(field) << ", actual "
                      << actual.at(field) << '\n';
    const auto& before = expected.at("cases");
    const auto& after = actual.at("cases");
    for (std::size_t c = 0; c < std::max(before.size(), after.size()); ++c) {
        if (c >= before.size() || c >= after.size()) {
            std::cerr << "case " << c << " missing from "
                      << (c >= before.size() ? "golden" : "observation") << '\n';
            continue;
        }
        const auto name = after[c].at("name").get<std::string>();
        if (before[c].at("name") != after[c].at("name") ||
            before[c].at("target") != after[c].at("target"))
            std::cerr << name << ": case name or target differs\n";
        if (before[c].at("terminated") != after[c].at("terminated"))
            std::cerr << name << ": termination differs\n";
        const auto& left = before[c].at("steps");
        const auto& right = after[c].at("steps");
        for (std::size_t step = 0; step < std::max(left.size(), right.size()); ++step) {
            if (step >= left.size() || step >= right.size() || left[step] != right[step])
                std::cerr << name << " step " << step << ": expected "
                          << (step < left.size() ? left[step].dump() : "<missing>") << ", actual "
                          << (step < right.size() ? right[step].dump() : "<missing>") << '\n';
        }
    }
    return false;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Compare greedy product tool/JSON mask walks with the pre-change golden.\n"
                         "Each step hashes the little-endian masks for no draft and a three-token\n"
                         "draft. Every grammar is compiled twice with forced cache eviction.\n"
                         "--write-golden PATH writes a new baseline; never use it to hide a\n"
                         "mask change when checking a dependency update.\n";
            return 0;
        }
        require(argc == 1 || (argc == 3 && std::string_view(argv[1]) == "--write-golden"),
                "usage: ninfer_qwen_grammar_mask_identity_test [--write-golden PATH]");
        const auto actual = observe();
        if (argc == 3) {
            for (const auto& test : actual.at("cases"))
                require(test.at("terminated").get<bool>(),
                        test.at("name").get<std::string>() + ": golden target did not reach EOS");
            std::ofstream output(argv[2], std::ios::binary);
            require(bool(output), "cannot open golden output");
            output << actual.dump(2) << '\n';
            require(bool(output), "cannot write golden output");
        } else {
            std::ifstream input(NINFER_SOURCE_DIR "/tests/fixtures/grammar/mask_identity.json");
            require(bool(input), "cannot open mask identity golden");
            Json expected;
            input >> expected;
            if (!compare(expected, actual)) return 1;
        }
        std::size_t steps = 0;
        for (const auto& test : actual.at("cases")) steps += test.at("steps").size();
        std::cout << "mask identity: " << actual.at("cases").size() << " cases, " << steps
                  << " steps, two compilations with eviction: equal\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
