#include <lux/engine/runtime/spatial_partition/SpatialDemandPlanner.hpp>

#include <lux/engine/core/extension_abi/StableId.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace lux::runtime::spatial_partition
{
    namespace
    {
        [[nodiscard]] bool uuidLess(
            const lux::entity_scene::EntitySectionId& lhs,
            const lux::entity_scene::EntitySectionId& rhs) noexcept
        {
            return lhs.value() < rhs.value();
        }

        [[nodiscard]] bool validSource(
            const SpatialDemandSourceId& source) noexcept
        {
            return source.isValid() &&
                lux::extensions::isCanonicalStableName(source.name());
        }

        [[nodiscard]] bool validChannel(
            const lux::entity_scene::DemandChannelId& channel) noexcept
        {
            return lux::entity_scene::isValidEntitySceneId(channel);
        }

        [[nodiscard]] bool recordHasChannel(
            const lux::entity_scene::EntitySectionRecord& record,
            const lux::entity_scene::DemandChannelId& channel) noexcept
        {
            return std::any_of(
                record.demand_channels.begin(),
                record.demand_channels.end(),
                [&channel](const auto& value)
                {
                    return lux::extensions::sameStableId(
                        value.view(), channel.view());
                });
        }

        [[nodiscard]] bool checkedAdd(
            std::uint64_t& value,
            std::uint64_t added) noexcept
        {
            if (value > std::numeric_limits<std::uint64_t>::max() - added)
                return false;
            value += added;
            return true;
        }
    }

    SpatialPartitionExp<SpatialDemandPlanner>
    SpatialDemandPlanner::create(
        EntitySectionRecordStore records,
        SpatialPartitionBudget budget)
    {
        if (!budget.valid())
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::INVALID_BUDGET});
        }
        return SpatialDemandPlanner{std::move(records), budget};
    }

    SpatialPartitionExp<SpatialDemandPlan>
    SpatialDemandPlanner::prepareReplace(
        SpatialDemandSourceUpdate update) const
    {
        if (!validSource(update.source) || update.generation == 0u)
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::INVALID_SOURCE,
                .source = std::move(update.source)});
        }
        if (!validChannel(update.channel))
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::INVALID_CHANNEL,
                .source = std::move(update.source)});
        }
        if (update.demands.empty())
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::EMPTY_DEMAND,
                .source = std::move(update.source)});
        }
        std::sort(
            update.demands.begin(), update.demands.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return uuidLess(lhs.section, rhs.section);
            });
        if (std::adjacent_find(
                update.demands.begin(), update.demands.end(),
                [](const auto& lhs, const auto& rhs)
                {
                    return lhs.section == rhs.section;
                }) != update.demands.end())
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::DUPLICATE_SECTION,
                .source = std::move(update.source)});
        }
        std::sort(
            update.records.begin(), update.records.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return uuidLess(lhs.id, rhs.id);
            });
        for (const auto& record : update.records)
        {
            if (!lux::entity_scene::validateEntitySectionRecord(record))
            {
                return lux::cxx::unexpected(SpatialPartitionFailure{
                    .code = ESpatialPartitionError::INVALID_DYNAMIC_RECORD,
                    .source = update.source,
                    .section = record.id});
            }
        }
        if (std::adjacent_find(
                update.records.begin(), update.records.end(),
                [](const auto& lhs, const auto& rhs)
                {
                    return lhs.id == rhs.id;
                }) != update.records.end())
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::INVALID_DYNAMIC_RECORD,
                .source = update.source});
        }

        SpatialDemandPlan plan;
        plan.base_revision_ = revision_;
        plan.sources_ = sources_;
        auto found = std::lower_bound(
            plan.sources_.begin(),
            plan.sources_.end(),
            std::string_view{update.source.name()},
            [](const auto& lhs, std::string_view rhs)
            {
                return lhs.id.name() < rhs;
            });
        if (found != plan.sources_.end() &&
            found->id.name() == update.source.name())
        {
            if (update.generation <= found->generation)
            {
                return lux::cxx::unexpected(SpatialPartitionFailure{
                    .code = ESpatialPartitionError::
                        STALE_SOURCE_GENERATION,
                    .source = std::move(update.source),
                    .requested = update.generation,
                    .available = found->generation});
            }
            *found = SpatialDemandPlan::SourceState{
                std::move(update.source),
                update.generation,
                std::move(update.channel),
                std::move(update.demands),
                std::move(update.records)};
        }
        else
        {
            plan.sources_.insert(
                found,
                SpatialDemandPlan::SourceState{
                    std::move(update.source),
                    update.generation,
                    std::move(update.channel),
                    std::move(update.demands),
                    std::move(update.records)});
        }
        auto rebuilt = rebuild(plan);
        if (!rebuilt)
            return lux::cxx::unexpected(std::move(rebuilt.error()));
        return plan;
    }

    SpatialPartitionExp<SpatialDemandPlan>
    SpatialDemandPlanner::prepareRemove(
        const SpatialDemandSourceId& source,
        std::uint64_t generation) const
    {
        if (!validSource(source) || generation == 0u)
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::INVALID_SOURCE,
                .source = source});
        }
        SpatialDemandPlan plan;
        plan.base_revision_ = revision_;
        plan.sources_ = sources_;
        const auto found = std::lower_bound(
            plan.sources_.begin(),
            plan.sources_.end(),
            std::string_view{source.name()},
            [](const auto& lhs, std::string_view rhs)
            {
                return lhs.id.name() < rhs;
            });
        if (found == plan.sources_.end() ||
            found->id.name() != source.name())
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::SOURCE_NOT_FOUND,
                .source = source});
        }
        if (generation != found->generation)
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::STALE_SOURCE_GENERATION,
                .source = source,
                .requested = generation,
                .available = found->generation});
        }
        plan.sources_.erase(found);
        auto rebuilt = rebuild(plan);
        if (!rebuilt)
            return lux::cxx::unexpected(std::move(rebuilt.error()));
        return plan;
    }

    SpatialPartitionExp<void>
    SpatialDemandPlanner::rebuild(SpatialDemandPlan& plan) const
    {
        struct Aggregate final
        {
            lux::entity_scene::EntitySectionRecord record;
            std::size_t references{0u};
            std::uint32_t priority{0u};
        };

        std::map<uuids::uuid, Aggregate> aggregate;
        std::map<
            uuids::uuid,
            const lux::entity_scene::EntitySectionRecord*> dynamic_records;
        for (const auto& source : plan.sources_)
        {
            for (const auto& record : source.records)
            {
                if (const auto* manifest_record = records_.find(record.id))
                {
                    if (*manifest_record != record)
                    {
                        return lux::cxx::unexpected(
                            SpatialPartitionFailure{
                                .code = ESpatialPartitionError::
                                    DYNAMIC_RECORD_CONFLICT,
                                .source = source.id,
                                .section = record.id});
                    }
                    continue;
                }
                const auto [found, inserted] = dynamic_records.emplace(
                    record.id.value(), &record);
                if (!inserted && *found->second != record)
                {
                    return lux::cxx::unexpected(SpatialPartitionFailure{
                        .code = ESpatialPartitionError::
                            DYNAMIC_RECORD_CONFLICT,
                        .source = source.id,
                        .section = record.id});
                }
            }
        }
        std::size_t source_references = 0u;
        for (const auto& source : plan.sources_)
        {
            // Dynamic metadata is owned by the source transaction that
            // issues the demand. Never let one source accidentally borrow a
            // record supplied only by another source: that would make removal
            // of the owner fail or leave the borrower with dangling address
            // semantics. Overlapping sources simply publish the same
            // canonical record independently (validated above).
            const auto resolveRecord = [this, &source](
                lux::entity_scene::EntitySectionId id) noexcept
                -> const lux::entity_scene::EntitySectionRecord*
            {
                const auto generated = std::lower_bound(
                    source.records.begin(), source.records.end(), id,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return uuidLess(lhs.id, rhs);
                    });
                if (generated != source.records.end() &&
                    generated->id == id)
                {
                    return &*generated;
                }
                return records_.find(id);
            };
            std::map<uuids::uuid, std::uint32_t> source_closure;
            for (const auto& demand : source.demands)
            {
                std::set<uuids::uuid> visiting;
                auto expand = [&](auto&& self,
                                  lux::entity_scene::EntitySectionId id,
                                  bool demanded)
                    -> SpatialPartitionExp<void>
                {
                    const auto* record = resolveRecord(id);
                    if (!record)
                    {
                        return lux::cxx::unexpected(
                            SpatialPartitionFailure{
                                .code = ESpatialPartitionError::
                                    SECTION_NOT_FOUND,
                                .source = source.id,
                                .section = id});
                    }
                    if (demanded &&
                        !recordHasChannel(*record, source.channel))
                    {
                        return lux::cxx::unexpected(
                            SpatialPartitionFailure{
                                .code = ESpatialPartitionError::
                                    CHANNEL_MISMATCH,
                                .source = source.id,
                                .section = id});
                    }
                    if (visiting.contains(id.value()))
                    {
                        return lux::cxx::unexpected(
                            SpatialPartitionFailure{
                                .code = ESpatialPartitionError::
                                    INVALID_DEPENDENCY,
                                .source = source.id,
                                .section = id});
                    }
                    const auto known = source_closure.find(id.value());
                    if (known != source_closure.end() &&
                        known->second >= demand.priority)
                    {
                        return {};
                    }
                    source_closure[id.value()] = demand.priority;
                    visiting.insert(id.value());
                    for (const auto& dependency : record->dependencies)
                    {
                        auto expanded = self(self, dependency, false);
                        if (!expanded)
                            return expanded;
                    }
                    visiting.erase(id.value());
                    return {};
                };
                auto expanded = expand(expand, demand.section, true);
                if (!expanded)
                    return lux::cxx::unexpected(std::move(expanded.error()));
            }
            for (const auto& record : source.records)
            {
                if (!source_closure.contains(record.id.value()))
                {
                    return lux::cxx::unexpected(
                        SpatialPartitionFailure{
                            .code = ESpatialPartitionError::
                                INVALID_DYNAMIC_RECORD,
                            .source = source.id,
                            .section = record.id});
                }
            }
            for (const auto& [id, priority] : source_closure)
            {
                const auto* record = resolveRecord(
                    lux::entity_scene::EntitySectionId{id});
                if (!record)
                    std::abort();
                auto found = aggregate.try_emplace(
                    id, Aggregate{*record, 0u, priority}).first;
                ++found->second.references;
                found->second.priority = std::max(
                    found->second.priority, priority);
                ++source_references;
            }
        }

        plan.residents_.clear();
        plan.residents_.reserve(aggregate.size());
        std::uint64_t decoded_bytes = 0u;
        std::uint64_t entity_count = 0u;
        for (auto& [id, value] : aggregate)
        {
            if (!checkedAdd(decoded_bytes, value.record.decoded_bytes) ||
                !checkedAdd(entity_count, value.record.entity_count))
            {
                return lux::cxx::unexpected(SpatialPartitionFailure{
                    .code = ESpatialPartitionError::BUDGET_OVERFLOW,
                    .section = value.record.id});
            }
            plan.residents_.push_back(SpatialResidentDemand{
                std::move(value.record),
                value.references,
                value.priority});
        }
        if (decoded_bytes > budget_.maximum_decoded_bytes)
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::
                    DECODED_BYTE_BUDGET_EXCEEDED,
                .requested = decoded_bytes,
                .available = budget_.maximum_decoded_bytes});
        }
        if (entity_count > budget_.maximum_entities)
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::ENTITY_BUDGET_EXCEEDED,
                .requested = entity_count,
                .available = budget_.maximum_entities});
        }
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::
                    PLAN_REVISION_EXHAUSTED});
        }
        plan.snapshot_ = SpatialDemandPlannerSnapshot{
            .revision = revision_ + 1u,
            .source_count = plan.sources_.size(),
            .dynamic_records = dynamic_records.size(),
            .resident_sections = plan.residents_.size(),
            .source_references = source_references,
            .decoded_bytes = decoded_bytes,
            .entity_count = entity_count,
            .maximum_decoded_bytes = budget_.maximum_decoded_bytes,
            .maximum_entities = budget_.maximum_entities};
        return {};
    }

    SpatialPartitionExp<void>
    SpatialDemandPlanner::commit(SpatialDemandPlan plan) noexcept
    {
        if (plan.base_revision_ != revision_)
        {
            return lux::cxx::unexpected(SpatialPartitionFailure{
                .code = ESpatialPartitionError::PLAN_STALE,
                .requested = plan.base_revision_,
                .available = revision_});
        }
        sources_ = std::move(plan.sources_);
        residents_ = std::move(plan.residents_);
        snapshot_ = plan.snapshot_;
        revision_ = snapshot_.revision;
        return {};
    }
}
