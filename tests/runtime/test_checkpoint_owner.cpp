// GPU-free catalog/Native ownership contract. Checkpoint leases and erase refusal use the
// production store; token records stand in for model state, not for lease lifetime behavior.
#include "models/common/context/checkpoint_store.h"
#include "runtime/engine/context_cache/resource_manager.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
namespace rt = ninfer::runtime;
using Handle = ninfer::models::context::CheckpointHandle;
using Role = rt::CheckpointRole;
using Key = std::vector<std::uint32_t>;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct Base {
    struct Advice {
        bool update_session_index = true;
        std::optional<std::uint64_t> session_key = 11;
        std::optional<std::uint64_t> reuse_scope = 11;
        std::optional<std::uint64_t> shared_reuse_scope = 9;
    } advice;
    struct Summary {
        std::uint32_t prompt_tokens;
        bool publish_continuation = true;
    };
    Key tokens;
    const Advice& context_cache() const { return advice; }
    Summary summary() const { return {static_cast<std::uint32_t>(tokens.size())}; }
    std::optional<Key> prefix_shortlist_key(std::uint32_t frontier) const {
        if (frontier > tokens.size()) { return std::nullopt; }
        return Key(tokens.begin(), tokens.begin() + frontier);
    }
};

struct Source {
    std::optional<Handle> checkpoint;
    std::uint32_t reused_tokens = 0;
    rt::PrefillWork remaining_work;
    std::vector<rt::ContextTransferRequirement> transfers;
    bool consume_source = false;
    bool take_private = false;
    std::vector<Handle> retired_points;
    std::vector<Handle> private_points;
};

struct Program {
    using ContextResources = rt::ContextResources<1>;
    struct Record {
        Key tokens;
        Role role;
    };
    struct Metadata {
        std::uint32_t frontier;
        Role role;
        bool leased;
    };
    struct Usage {
        ContextResources capacity{.state_slots = 64, .kv_page_groups = {64}, .host_bytes = 4096};
    };
    ninfer::models::context::CheckpointStore<Record> checkpoints{64};
    std::uint32_t refused_releases = 0;

    Handle add(Key tokens, Role role) {
        const auto slot = checkpoints.reserve();
        require(slot.has_value(), "fixture exhausted checkpoint descriptors");
        checkpoints.publish(*slot, {std::move(tokens), role});
        return *slot;
    }
    bool valid_checkpoint(Handle handle) const { return checkpoints.valid(handle); }
    bool release_checkpoint(Handle handle) {
        const bool released = checkpoints.erase(handle);
        refused_releases += !released;
        return released;
    }
    Metadata checkpoint_metadata(Handle handle) const {
        const auto& record = checkpoints.at(handle);
        return {static_cast<std::uint32_t>(record.tokens.size()), record.role,
                checkpoints[handle.index].pins != 0};
    }
    Key checkpoint_key(Handle handle, std::uint32_t frontier) const {
        const auto& tokens = checkpoints.at(handle).tokens;
        require(frontier <= tokens.size(), "invalid checkpoint prefix");
        return {tokens.begin(), tokens.begin() + frontier};
    }
    Usage physical_usage() const { return {}; }
    ContextResources checkpoint_footprint(std::span<const Handle> points) const {
        ContextResources result;
        for (const auto point : points) {
            (void)checkpoints.at(point);
            ++result.state_slots;
        }
        return result;
    }
    // The catalog names this type, but this test never runs resource-pressure planning.
    struct ReclaimPlan {};
    ReclaimPlan plan_reclaim(std::span<const Handle>, std::span<const Handle>,
                             ContextResources) const;

    std::optional<Source> inspect_source(const Base& base, std::optional<Handle> selected,
                                         bool consume, std::span<const Handle> carried,
                                         std::span<const Handle> retired) const {
        Source source{.checkpoint = selected, .consume_source = consume};
        if (selected) {
            const auto frontier = checkpoint_metadata(*selected).frontier;
            if (base.prefix_shortlist_key(frontier) != checkpoint_key(*selected, frontier)) {
                return std::nullopt;
            }
            source.reused_tokens = frontier;
            // Like Qwen3.5 inspection: carry only compatible earlier private recovery points,
            // never the deeper endpoint that the sibling fork is rewinding away from.
            for (const auto point : carried) {
                const auto metadata = checkpoint_metadata(point);
                if (metadata.role != Role::Continuation && metadata.frontier <= frontier &&
                    base.prefix_shortlist_key(metadata.frontier) ==
                        checkpoint_key(point, metadata.frontier)) {
                    source.private_points.push_back(point);
                }
            }
        }
        source.retired_points.assign(retired.begin(), retired.end());
        source.remaining_work.tokens = base.tokens.size() - source.reused_tokens;
        return source;
    }
};

struct Model {
    using Program = ::Program;
    using RequestBasePlan = Base;
    using CheckpointHandle = Handle;
    using SourceCandidate = Source;
    using CacheSessionKey = std::uint64_t;
};
using Cache = rt::ResourceManager<Model>;

Cache::SourceChoice select(Cache& cache, Program& program, const Base& base, Handle handle) {
    const auto choices = cache.candidates(program, base);
    const auto found = std::find_if(choices.begin(), choices.end(), [&](const auto& choice) {
        return choice.source.checkpoint == handle;
    });
    require(found != choices.end(), "sibling lost its compatible anchor");
    return *found;
}

void fork_after_resume(bool imported, bool pin_anchor) {
    Program program;
    Cache cache(true, {.prefill = {.token_ns_q32 = rt::kContextCostQ32One}});
    const Base endpoint{.tokens = {1, 2, 3, 4, 5, 6}};
    const Base sibling{.tokens = {1, 2, 9}};
    const auto anchor = program.add({1, 2}, Role::LongAnchor);
    const auto deeper = program.add(endpoint.tokens, Role::Continuation);
    const std::array points{anchor, deeper};
    Cache::OwnerToken original;
    if (imported) {
        auto publication = cache.prepare_import_publication(11, 9, "session", "restored", 2);
        cache.publish_import(program, std::move(publication), points, 1);
        original = select(cache, program, sibling, anchor).owner;
        require(!select(cache, program, sibling, anchor).take_over,
                "acknowledged imported endpoint was already consumable");
    } else {
        original = cache.adopt(program, {}, endpoint, 1, points);
        cache.finish(program, original, endpoint, 1);
    }

    // An off-worker export can still read this owner when a newer endpoint is acknowledged.
    // Use real Native pins: valid checkpoints must refuse erasure while the reader lives.
    auto endpoint_reader = program.checkpoints.lease(deeper);
    auto anchor_reader = pin_anchor ? program.checkpoints.lease(anchor)
                                    : decltype(endpoint_reader){};
    require(program.valid_checkpoint(deeper) && !program.checkpoints.erasable(deeper),
            "fixture did not hold the Native checkpoint reader");

    if (imported) {
        // Endpoint resume forks the protected imported owner; its new stored response
        // supersedes the imported identity without retiring the export's Native leases.
        const auto resume_choice = select(cache, program, endpoint, deeper);
        const auto resumed = cache.adopt(program, resume_choice, endpoint, 2);
        const auto next = program.add({1, 2, 3, 4, 5, 6, 7}, Role::Continuation);
        cache.publish(program, resumed, next);
        cache.set_checkpoint_identity(resumed, "session", "resumed");
        cache.finish(program, resumed, endpoint, 2);
        cache.supersede_durable("session", 2);
    }

    auto choice = select(cache, program, sibling, anchor);
    cache.retain_source(program, 3, choice);
    // Native bind: retire only quoted unleased points; fork clones compatible anchors.
    for (const auto point : choice.source.retired_points) {
        require(program.release_checkpoint(point), "binding quoted a pinned retirement");
    }
    auto carried = choice.source.private_points;
    if (!choice.source.take_private) {
        for (auto& point : carried) {
            const auto record = program.checkpoints.at(point);
            point = program.add(record.tokens, record.role);
        }
    }
    cache.binding_started(3, choice.source.retired_points, std::nullopt);
    // Before the fix, replace_points drops deeper's last catalog reference and calls
    // the real CheckpointStore::erase while endpoint_reader still pins it: exact native error.
    const auto branch = cache.adopt(program, choice, sibling, 3, carried,
                                    choice.source.retired_points);
    cache.release_source(program, 3);
    require(branch != original && !choice.take_over && !choice.source.consume_source &&
                !choice.source.take_private && choice.source.retired_points.empty(),
            "pinned owner was transferred instead of independently forked");
    require(program.refused_releases == 0 && program.valid_checkpoint(deeper) &&
                cache.points(original) == std::vector<Handle>(points.begin(), points.end()),
            "fork dropped the reader's original catalog owner");
    require(carried.size() == 1 && carried.front() != anchor &&
                cache.points(branch) == carried,
            "sibling did not receive its own compatible anchor");
    cache.abandon(program, branch);
    require(!program.valid_checkpoint(carried.front()) && program.valid_checkpoint(anchor),
            "sibling cancellation damaged the original export");

    anchor_reader.reset();
    endpoint_reader.reset();
    choice = select(cache, program, sibling, anchor);
    require(choice.take_over && choice.source.take_private && choice.source.consume_source &&
                choice.source.retired_points == std::vector<Handle>{deeper},
            "retired export permanently disabled normal owner takeover");
    require(cache.erase(program, deeper) && cache.erase(program, anchor),
            "unleased predecessor could not be reclaimed");
    require(!program.valid_checkpoint(deeper) && !program.valid_checkpoint(anchor),
            "checkpoint owner leaked after its reader retired");
}
} // namespace

int main() {
    try {
        for (const bool imported : {true, false}) {
            for (const bool pin_anchor : {true, false}) {
                fork_after_resume(imported, pin_anchor);
            }
        }
        std::cout << "checkpoint owner contracts passed (4 lease/supersession cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
