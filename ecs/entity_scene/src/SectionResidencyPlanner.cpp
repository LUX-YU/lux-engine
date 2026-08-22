#include <lux/engine/ecs/entity_scene/residency/SectionResidencyPlanner.hpp>

#include <lux/engine/resource/asset/storage/VirtualPath.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <variant>

namespace lux::ecs::entity_scene::residency
{
    namespace
    {
        [[nodiscard]] bool uuidLess(
            const lux::ecs::scene_format::EntitySectionId& lhs,
            const lux::ecs::scene_format::EntitySectionId& rhs) noexcept
        {
            return lhs.value() < rhs.value();
        }

        [[nodiscard]] bool validSource(
            const SectionDemandSourceId& source) noexcept
        {
            return source.isValid() &&
                lux::ecs::scene_format::isCanonicalStableName(source.name());
        }

        [[nodiscard]] bool validChannel(
            const lux::ecs::scene_format::DemandChannelId& channel) noexcept
        {
            return lux::ecs::scene_format::isValidDemandChannelId(channel);
        }

        [[nodiscard]] bool recordHasChannel(
            const lux::ecs::scene_format::SectionRecord& record,
            const lux::ecs::scene_format::DemandChannelId& channel) noexcept
        {
            return std::any_of(
                record.demand_channels.begin(),
                record.demand_channels.end(),
                [&channel](const auto& value)
                {
                    return value.view() == channel.view();
                });
        }

        [[nodiscard]] bool validSectionResidencyRecordImpl(
            const lux::ecs::scene_format::SectionRecord& record) noexcept
        {
            using namespace lux::ecs::scene_format;

            constexpr std::uint64_t maximum_section_bytes =
                1024ull * 1024ull * 1024ull;
            constexpr std::size_t maximum_name_bytes = 4096u;
            constexpr std::size_t maximum_dependencies = 4096u;
            constexpr std::size_t maximum_requirements = 65536u;
            constexpr std::size_t maximum_generator_parameter_bytes =
                4u * 1024u * 1024u;
            constexpr std::uint32_t maximum_entities = 4u * 1024u * 1024u;

            if (record.id.empty() ||
                record.content_digest ==
                    lux::cxx::algorithm::Sha256Digest{} ||
                record.encoded_bytes == 0u ||
                record.decoded_bytes == 0u ||
                record.encoded_bytes > maximum_section_bytes ||
                record.decoded_bytes > maximum_section_bytes ||
                record.entity_count > maximum_entities ||
                record.dependencies.size() > maximum_dependencies ||
                record.demand_channels.size() > maximum_requirements ||
                record.required_components.size() > maximum_requirements)
            {
                return false;
            }
            if ((record.compression != SectionCompression::NONE &&
                 record.compression != SectionCompression::ZSTD) ||
                (record.compression == SectionCompression::NONE &&
                 record.encoded_bytes != record.decoded_bytes))
            {
                return false;
            }

            if (const auto* stored =
                    std::get_if<StoredSectionSource>(&record.source))
            {
                if (stored->content_path.size() > maximum_name_bytes ||
                    !lux::asset::VirtualPath::parse(
                        stored->content_path).has_value())
                {
                    return false;
                }
            }
            else if (const auto* generated =
                         std::get_if<GeneratedSectionSource>(&record.source))
            {
                if (!isValidSectionGeneratorId(generated->generator) ||
                    generated->generator.name().size() >
                        maximum_name_bytes ||
                    record.compression != SectionCompression::NONE ||
                    generated->parameters.size() >
                        maximum_generator_parameter_bytes)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            for (std::size_t index = 0u;
                 index < record.dependencies.size();
                 ++index)
            {
                const auto dependency = record.dependencies[index];
                if (dependency.empty() || dependency == record.id ||
                    (index != 0u &&
                     !uuidLess(record.dependencies[index - 1u], dependency)))
                {
                    return false;
                }
            }
            for (std::size_t index = 0u;
                 index < record.demand_channels.size();
                 ++index)
            {
                const auto& channel = record.demand_channels[index];
                if (!isValidDemandChannelId(channel) ||
                    channel.name().size() > maximum_name_bytes ||
                    (index != 0u &&
                     record.demand_channels[index - 1u].name() >=
                         channel.name()))
                {
                    return false;
                }
            }
            for (std::size_t index = 0u;
                 index < record.required_components.size();
                 ++index)
            {
                const auto& component = record.required_components[index];
                if (!lux::ecs::isValidComponentSchemaId(component.id) ||
                    component.id.name.size() > maximum_name_bytes ||
                    component.schema_version == 0u ||
                    (index != 0u &&
                     record.required_components[index - 1u].id.name >=
                         component.id.name))
                {
                    return false;
                }
            }
            return true;
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

        [[nodiscard]] const lux::ecs::scene_format::SectionRecord*
        findRecord(
            std::span<const lux::ecs::scene_format::SectionRecord> records,
            lux::ecs::scene_format::EntitySectionId id) noexcept
        {
            const auto found = std::lower_bound(
                records.begin(),
                records.end(),
                id,
                [](const auto& record, const auto& target)
                {
                    return uuidLess(record.id, target);
                });
            return found != records.end() && found->id == id
                ? &*found
                : nullptr;
        }
    }

    bool isValidSectionResidencyRecord(
        const lux::ecs::scene_format::SectionRecord& record) noexcept
    {
        return validSectionResidencyRecordImpl(record);
    }

    SectionResidencyExp<SectionResidencyPlanner>
    SectionResidencyPlanner::create(
        std::span<const lux::ecs::scene_format::SectionRecord> records,
        SectionResidencyBudget budget)
    {
        if (!budget.valid())
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::INVALID_BUDGET});
        }
        return SectionResidencyPlanner{records, budget};
    }

    SectionResidencyExp<SectionResidencyPlan>
    SectionResidencyPlanner::prepareReplace(
        SectionDemandSourceUpdate update) const
    {
        if (!validSource(update.source) || update.generation == 0u)
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::INVALID_SOURCE,
                .source = std::move(update.source)});
        }
        if (!validChannel(update.channel))
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::INVALID_CHANNEL,
                .source = std::move(update.source)});
        }
        if (update.demands.empty())
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::EMPTY_DEMAND,
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
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::DUPLICATE_SECTION,
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
            if (!isValidSectionResidencyRecord(record))
            {
                return lux::cxx::unexpected(SectionResidencyFailure{
                    .code = ESectionResidencyError::INVALID_DYNAMIC_RECORD,
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
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::INVALID_DYNAMIC_RECORD,
                .source = update.source});
        }

        SectionResidencyPlan plan;
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
                return lux::cxx::unexpected(SectionResidencyFailure{
                    .code = ESectionResidencyError::
                        STALE_SOURCE_GENERATION,
                    .source = std::move(update.source),
                    .requested = update.generation,
                    .available = found->generation});
            }
            *found = SectionResidencyPlan::SourceState{
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
                SectionResidencyPlan::SourceState{
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

    SectionResidencyExp<SectionResidencyPlan>
    SectionResidencyPlanner::prepareRemove(
        const SectionDemandSourceId& source,
        std::uint64_t generation) const
    {
        if (!validSource(source) || generation == 0u)
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::INVALID_SOURCE,
                .source = source});
        }
        SectionResidencyPlan plan;
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
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::SOURCE_NOT_FOUND,
                .source = source});
        }
        if (generation != found->generation)
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::STALE_SOURCE_GENERATION,
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

    SectionResidencyExp<void>
    SectionResidencyPlanner::rebuild(SectionResidencyPlan& plan) const
    {
        struct Aggregate final
        {
            lux::ecs::scene_format::SectionRecord record;
            std::size_t references{0u};
            std::uint32_t priority{0u};
        };

        std::map<uuids::uuid, Aggregate> aggregate;
        std::map<
            uuids::uuid,
            const lux::ecs::scene_format::SectionRecord*> dynamic_records;
        for (const auto& source : plan.sources_)
        {
            for (const auto& record : source.records)
            {
                if (const auto* manifest_record = findRecord(
                        records_,
                        record.id))
                {
                    if (*manifest_record != record)
                    {
                        return lux::cxx::unexpected(
                            SectionResidencyFailure{
                                .code = ESectionResidencyError::
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
                    return lux::cxx::unexpected(SectionResidencyFailure{
                        .code = ESectionResidencyError::
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
                lux::ecs::scene_format::EntitySectionId id) noexcept
                -> const lux::ecs::scene_format::SectionRecord*
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
                return findRecord(records_, id);
            };
            std::map<uuids::uuid, std::uint32_t> source_closure;
            for (const auto& demand : source.demands)
            {
                std::set<uuids::uuid> visiting;
                auto expand = [&](auto&& self,
                                  lux::ecs::scene_format::EntitySectionId id,
                                  bool demanded)
                    -> SectionResidencyExp<void>
                {
                    const auto* record = resolveRecord(id);
                    if (!record)
                    {
                        return lux::cxx::unexpected(
                            SectionResidencyFailure{
                                .code = ESectionResidencyError::
                                    SECTION_NOT_FOUND,
                                .source = source.id,
                                .section = id});
                    }
                    if (demanded &&
                        !recordHasChannel(*record, source.channel))
                    {
                        return lux::cxx::unexpected(
                            SectionResidencyFailure{
                                .code = ESectionResidencyError::
                                    CHANNEL_MISMATCH,
                                .source = source.id,
                                .section = id});
                    }
                    if (visiting.contains(id.value()))
                    {
                        return lux::cxx::unexpected(
                            SectionResidencyFailure{
                                .code = ESectionResidencyError::
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
                        SectionResidencyFailure{
                            .code = ESectionResidencyError::
                                INVALID_DYNAMIC_RECORD,
                            .source = source.id,
                            .section = record.id});
                }
            }
            for (const auto& [id, priority] : source_closure)
            {
                const auto* record = resolveRecord(
                    lux::ecs::scene_format::EntitySectionId{id});
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
                return lux::cxx::unexpected(SectionResidencyFailure{
                    .code = ESectionResidencyError::BUDGET_OVERFLOW,
                    .section = value.record.id});
            }
            plan.residents_.push_back(SectionResidentDemand{
                std::move(value.record),
                value.references,
                value.priority});
        }
        if (decoded_bytes > budget_.maximum_decoded_bytes)
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::
                    DECODED_BYTE_BUDGET_EXCEEDED,
                .requested = decoded_bytes,
                .available = budget_.maximum_decoded_bytes});
        }
        if (entity_count > budget_.maximum_entities)
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::ENTITY_BUDGET_EXCEEDED,
                .requested = entity_count,
                .available = budget_.maximum_entities});
        }
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::
                    PLAN_REVISION_EXHAUSTED});
        }
        plan.snapshot_ = SectionResidencyPlannerSnapshot{
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

    SectionResidencyExp<void>
    SectionResidencyPlanner::commit(SectionResidencyPlan plan) noexcept
    {
        if (plan.base_revision_ != revision_)
        {
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::PLAN_STALE,
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
