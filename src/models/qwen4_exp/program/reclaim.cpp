#include "models/qwen4_exp/program/internal.h"

#include <algorithm>
#include <unordered_set>

namespace ninfer::models::qwen4_exp::detail {
namespace {
template <class T>
void append_unique(std::vector<T>& out, const T& value) {
    if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
}

bool same_prefix(const Checkpoint& a, const Checkpoint& b, std::uint32_t f) {
    const auto& x = a.snapshot.identity();
    const auto& y = b.snapshot.identity();
    return x.tag == y.tag && x.tokens.size() >= f && y.tokens.size() >= f &&
           std::equal(x.tokens.begin(), x.tokens.begin() + f, y.tokens.begin()) &&
           x.positions.prefix_equals(y.positions, f);
}
} // namespace

std::vector<Image> ProgramImpl::checkpoint_images(std::span<const CheckpointHandle> records) const {
    std::vector<Image> out;
    std::unordered_set<std::uint32_t> seen;
    for (const auto h : records)
        if (checkpoints.valid(h)) {
            const auto image = checkpoints.at(h).snapshot.continuation().image;
            if (seen.insert(resources.state_store->descriptor_index(image)).second)
                out.push_back(image);
        }
    return out;
}

std::vector<context::LogicalKVPageHandle>
ProgramImpl::checkpoint_pages(std::span<const CheckpointHandle> records, std::uint32_t pool) const {
    std::vector<context::LogicalKVPageHandle> out;
    std::unordered_set<std::uint32_t> seen;
    for (const auto h : records)
        if (checkpoints.valid(h)) {
            const auto& prefix = checkpoints.at(h).snapshot.prefix(pool);
            for (std::uint32_t i = 0; i < (prefix.frontier() + 63) / 64; ++i) {
                const auto page = resources.pool(pool).histories.logical_page(prefix.history(), i);
                if (seen.insert(resources.pool(pool).pages.descriptor_index(page)).second)
                    out.push_back(page);
            }
        }
    return out;
}

ContextResources ProgramImpl::footprint(std::span<const CheckpointHandle> input,
                                        bool released) const {
    std::vector<CheckpointHandle> records;
    for (const auto h : input)
        if (checkpoints.valid(h)) append_unique(records, h);
    ContextResources out;
    const auto& states = *resources.state_store;
    for (const auto image : checkpoint_images(records)) {
        const auto references = std::count_if(records.begin(), records.end(), [&](auto h) {
            return checkpoints.at(h).snapshot.continuation().image == image;
        });
        if (released &&
            (references != states.checkpoint_references(image) || states.source_pins(image)))
            continue;
        out.state_slots += states.device_resident(image);
        if (states.host_resident(image))
            out.host_bytes += resources.states->storage().image_bytes();
    }
    for (std::uint16_t pool = 0; pool < resources.pool_count(); ++pool) {
        const auto& storage = resources.pool(pool);
        std::vector<context::LogicalKVPageHandle> pages;
        if (released) {
            std::vector<const context::KVPrefixRef*> prefixes;
            for (const auto h : records)
                prefixes.push_back(&checkpoints.at(h).snapshot.prefix(pool));
            pages = storage.histories.released_pages(prefixes);
        } else
            pages = checkpoint_pages(records, pool);
        for (const auto page : pages) {
            if (released && storage.pages.source_pins(page)) continue;
            out.kv_page_groups[pool] += storage.pages.device_resident(page);
            if (storage.pages.host_resident(page)) out.host_bytes += pools[pool].host_page_stride;
        }
    }
    return out;
}

std::size_t ProgramImpl::host_released(std::span<const CheckpointHandle> records) const {
    return footprint(records, true).host_bytes;
}

std::uint64_t ProgramImpl::recovery_loss(std::span<const CheckpointHandle> removed,
                                         std::span<const CheckpointHandle> kept) const {
    std::uint64_t loss = 0;
    std::vector<CheckpointHandle> maximal;
    for (auto h : removed) {
        if (!checkpoints.valid(h)) continue;
        const auto& candidate = checkpoints.at(h);
        if (std::any_of(maximal.begin(), maximal.end(), [&](auto other) {
                return checkpoints.at(other).frontier() >= candidate.frontier() &&
                       same_prefix(candidate, checkpoints.at(other), candidate.frontier());
            }))
            continue;
        std::erase_if(maximal, [&](auto other) {
            return checkpoints.at(other).frontier() <= candidate.frontier() &&
                   same_prefix(candidate, checkpoints.at(other), checkpoints.at(other).frontier());
        });
        maximal.push_back(h);
    }
    for (auto h : maximal) {
        const auto& candidate   = checkpoints.at(h);
        std::uint32_t recovered = 0;
        for (auto other : kept)
            if (checkpoints.valid(other)) {
                const auto& source = checkpoints.at(other);
                if (source.frontier() <= candidate.frontier() &&
                    same_prefix(source, candidate, source.frontier()))
                    recovered = std::max(recovered, source.frontier());
            }
        loss += candidate.frontier() - recovered;
    }
    return loss;
}

std::shared_ptr<Demotion> ProgramImpl::demotion(std::span<const CheckpointHandle> records,
                                                ContextResources shortage) const {
    if (!resources.host_context) return {};
    auto out   = std::make_shared<Demotion>();
    out->owner = this;
    for (const auto h : records)
        if (checkpoints.valid(h)) append_unique(out->records, h);
    if (shortage.state_slots)
        for (const auto image : checkpoint_images(out->records))
            if (resources.state_store->device_resident(image) &&
                !resources.state_store->source_pins(image))
                out->images.push_back(image);
    for (std::uint16_t pool = 0; pool < resources.pool_count(); ++pool)
        if (shortage.kv_page_groups[pool]) {
            const auto& pages = resources.pool(pool).pages;
            for (const auto page : checkpoint_pages(out->records, pool))
                if (pages.device_resident(page) && !pages.source_pins(page) &&
                    !pages.active_history_references(page))
                    out->pages[pool].push_back(page);
        }
    return out->empty() ? nullptr : out;
}

ContextDemotion ProgramImpl::describe_demotion(std::shared_ptr<const Demotion> plan) const {
    ContextDemotion out;
    out.sources              = plan->records;
    out.released.state_slots = plan->images.size();
    for (const auto image : plan->images)
        if (!resources.state_store->host_resident(image))
            out.host_bytes += resources.states->storage().image_bytes();
    for (std::uint16_t pool = 0; pool < resources.pool_count(); ++pool) {
        out.released.kv_page_groups[pool] = plan->pages[pool].size();
        for (const auto page : plan->pages[pool])
            if (!resources.pool(pool).pages.host_replica_current(page))
                out.host_bytes += pools[pool].host_page_stride;
    }
    out.impl = std::move(plan);
    return out;
}

bool ProgramImpl::prepare_demotion(context::MigrationBatch& migration, const Demotion& plan) {
    std::vector<Image> redundant;
    for (const auto image : plan.images) {
        auto& states = *resources.state_store;
        if (!states.valid(image) || !states.device_resident(image) || states.source_pins(image))
            return false;
        if (states.host_resident(image))
            redundant.push_back(image);
        else {
            auto transfer = states.reserve_device_to_host(image);
            if (!transfer) return false;
            migration.add_state_copy(std::move(*transfer), false);
        }
    }
    std::array<std::vector<context::LogicalKVPageHandle>, 2> redundant_pages;
    for (std::uint16_t pool = 0; pool < resources.pool_count(); ++pool) {
        auto& store = resources.pool(pool).pages;
        std::vector<context::LogicalKVPageHandle> pages;
        for (auto page : plan.pages[pool]) {
            if (!store.valid(page) || !store.device_resident(page) || store.source_pins(page) ||
                store.active_history_references(page))
                return false;
            if (store.host_replica_current(page))
                redundant_pages[pool].push_back(page);
            else
                pages.push_back(page);
        }
        if (!pages.empty()) {
            auto target = resources.host_extents->prepare(store, pages);
            if (!target) return false;
            migration.add_backup({pool}, store, std::move(pages), std::move(*target), true);
        }
    }
    for (const auto image : redundant)
        if (!resources.state_store->drop_device_replica(image))
            throw std::logic_error("state demotion lost exclusivity");
    for (std::uint16_t pool = 0; pool < resources.pool_count(); ++pool)
        for (const auto page : redundant_pages[pool])
            if (!resources.pool(pool).pages.drop_device_replica(page))
                throw std::logic_error("KV demotion lost exclusivity");
    return true;
}

} // namespace ninfer::models::qwen4_exp::detail

namespace ninfer::models::qwen4_exp {

ContextResources Program::checkpoint_footprint(std::span<const CheckpointHandle> h) const {
    return impl_->footprint(h, false);
}

std::size_t Program::host_bytes_released(std::span<const CheckpointHandle> h) const {
    return impl_->host_released(h);
}

std::uint64_t Program::checkpoint_recovery_loss(std::span<const CheckpointHandle> removed,
                                                std::span<const CheckpointHandle> kept) const {
    return impl_->recovery_loss(removed, kept);
}

CheckpointSummary Program::checkpoint_summary(CheckpointHandle h) const {
    CheckpointSummary out;
    static_cast<CheckpointMetadata&>(out) = checkpoint_metadata(h);
    out.key                               = checkpoint_key(h, out.frontier);
    out.evictable_resources               = impl_->footprint({&h, 1}, true);
    return out;
}

std::vector<ContextRelease> Program::plan_releases(std::span<const CheckpointHandle> allowed,
                                                   std::span<const CheckpointHandle> excluded,
                                                   ContextResources shortage) const {
    const auto& p = *impl_;
    std::vector<CheckpointHandle> records;
    for (const auto h : allowed)
        if (p.checkpoints.erasable(h) &&
            std::find(excluded.begin(), excluded.end(), h) == excluded.end())
            records.push_back(h);
    std::vector<ContextRelease> out;
    const auto add = [&](std::vector<CheckpointHandle> owners) {
        std::sort(owners.begin(), owners.end(), [](auto a, auto b) { return a.index < b.index; });
        if (owners.empty() ||
            std::any_of(out.begin(), out.end(), [&](const auto& a) { return a.sources == owners; }))
            return;
        auto released = p.footprint(owners, true);
        if (released.useful(shortage)) out.push_back({std::move(owners), released});
    };
    for (const auto h : records) add({h});
    if (shortage.state_slots || shortage.host_bytes) {
        for (const auto image : p.checkpoint_images(records)) {
            std::vector<CheckpointHandle> owners;
            for (auto h : records)
                if (p.checkpoints.at(h).snapshot.continuation().image == image) owners.push_back(h);
            add(std::move(owners));
        }
    }
    for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
        if (shortage.kv_page_groups[pool] || shortage.host_bytes) {
            for (const auto page : p.checkpoint_pages(records, pool)) {
                std::vector<CheckpointHandle> owners;
                for (auto h : records) {
                    const auto pages = p.checkpoint_pages({&h, 1}, pool);
                    if (std::find(pages.begin(), pages.end(), page) != pages.end())
                        owners.push_back(h);
                }
                add(std::move(owners));
            }
        }
    return out;
}

ContextReclaimPlan Program::plan_reclaim(std::span<const CheckpointHandle> allowed,
                                         std::span<const CheckpointHandle> excluded,
                                         ContextResources shortage) const {
    auto& p = *impl_;
    ContextReclaimPlan out;
    out.impl     = std::make_shared<detail::Reclaim>(detail::Reclaim{&p});
    out.releases = plan_releases(allowed, excluded, shortage);
    for (const auto h : allowed) {
        if (!p.checkpoints.erasable(h) ||
            std::find(excluded.begin(), excluded.end(), h) != excluded.end())
            continue;
        auto plan = p.demotion({&h, 1}, shortage);
        if (!plan) continue;
        // A shared physical move names every holder; excluded holders veto the move.
        for (std::uint32_t i = 0; i < p.checkpoints.size(); ++i) {
            const auto other = p.checkpoints.handle(i);
            if (!p.checkpoints.valid(other) || other == h) continue;
            const auto image = p.checkpoints.at(other).snapshot.continuation().image;
            std::array<std::vector<context::LogicalKVPageHandle>, 2> pages;
            bool shares =
                std::find(plan->images.begin(), plan->images.end(), image) != plan->images.end();
            for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
                pages[pool] = p.checkpoint_pages({&other, 1}, pool);
                shares |= std::any_of(pages[pool].begin(), pages[pool].end(), [&](auto page) {
                    return std::find(plan->pages[pool].begin(), plan->pages[pool].end(), page) !=
                           plan->pages[pool].end();
                });
            }
            if (!shares) continue;
            if (std::find(allowed.begin(), allowed.end(), other) == allowed.end() ||
                std::find(excluded.begin(), excluded.end(), other) != excluded.end() ||
                !p.checkpoints.erasable(other)) {
                std::erase(plan->images, image);
                for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
                    std::erase_if(plan->pages[pool], [&](auto page) {
                        return std::find(pages[pool].begin(), pages[pool].end(), page) !=
                               pages[pool].end();
                    });
            } else if (std::find(plan->records.begin(), plan->records.end(), other) ==
                       plan->records.end())
                plan->records.push_back(other);
        }
        if (!plan->empty()) out.demotions.push_back(p.describe_demotion(std::move(plan)));
    }
    return out;
}

ContextDemotionBatch ContextReclaimPlan::begin_kv_batch(ContextResources) const {
    auto out   = std::make_shared<detail::Demotion>();
    out->owner = impl->owner;
    return ContextDemotionBatch(std::move(out));
}

std::uint64_t ContextReclaimPlan::recovery_loss(std::span<const CheckpointHandle> removed,
                                                std::span<const CheckpointHandle> kept) const {
    return impl->owner->recovery_loss(removed, kept);
}

bool ContextDemotionBatch::append(const ContextDemotion& other) {
    if (!other.impl || plan_->owner != other.impl->owner) return false;
    const auto append = [](auto& values, const auto& additions) {
        for (const auto value : additions)
            if (std::find(values.begin(), values.end(), value) == values.end())
                values.push_back(value);
    };
    append(plan_->images, other.impl->images);
    for (std::uint16_t pool = 0; pool < plan_->owner->resources.pool_count(); ++pool)
        append(plan_->pages[pool], other.impl->pages[pool]);
    append(plan_->records, other.impl->records);
    return true;
}

bool ContextDemotionBatch::covers(const ContextRelease& release) const {
    return std::all_of(release.sources.begin(), release.sources.end(), [&](auto h) {
        return std::find(plan_->records.begin(), plan_->records.end(), h) != plan_->records.end();
    });
}

std::optional<ContextDemotion> ContextDemotionBatch::finish() const {
    if (plan_->empty()) return {};
    return plan_->owner->describe_demotion(plan_);
}

bool Program::start_demote(const ContextDemotion& quote) {
    auto& p = *impl_;
    if (p.operation || p.pending_transaction || !quote.impl || quote.impl->owner != &p ||
        !p.resources.host_context || quote.host_bytes > p.resources.host_context->free_bytes())
        return false;
    p.operation = std::make_unique<detail::ContextOperation>(p.resources,
                                                             detail::DemoteOperation{quote.impl});
    try {
        auto& migration = p.operation->migration;
        if (!p.prepare_demotion(migration, *quote.impl)) {
            p.cancel_operation();
            return false;
        }
        migration.submit();
        return true;
    } catch (...) {
        p.cancel_operation();
        throw;
    }
}

std::size_t Program::release_redundant_host(std::span<const CheckpointHandle> excluded,
                                            std::optional<SequenceHandle> keep) {
    auto& p = *impl_;
    if (!p.resources.host_context) return 0;
    const auto before = p.resources.host_context->occupied_bytes();
    const auto images = p.checkpoint_images(excluded);
    std::vector<CheckpointHandle> all;
    for (std::uint32_t i = 0; i < p.checkpoints.size(); ++i)
        if (p.checkpoints.valid(p.checkpoints.handle(i))) all.push_back(p.checkpoints.handle(i));
    for (const auto image : p.checkpoint_images(all)) {
        if (std::find(images.begin(), images.end(), image) != images.end() ||
            (keep && p.valid(*keep) && p.sequence(*keep).continuation.image == image))
            continue;
        (void)p.resources.state_store->drop_host_replica(image);
    }
    for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
        const auto protected_pages = p.checkpoint_pages(excluded, pool);
        auto& store                = p.resources.pool(pool).pages;
        std::vector<context::LogicalKVPageHandle> drop;
        for (const auto page : p.checkpoint_pages(all, pool))
            if (std::find(protected_pages.begin(), protected_pages.end(), page) ==
                    protected_pages.end() &&
                store.host_resident(page) &&
                store.device_covers(page, store.required_columns(page)) &&
                p.resources.host_extents->can_release_page_replica(store, page))
                drop.push_back(page);
        if (!drop.empty()) (void)p.resources.host_extents->release_page_replicas(store, drop);
    }
    return before - p.resources.host_context->occupied_bytes();
}

} // namespace ninfer::models::qwen4_exp
