#include <lux/engine/runtime/scene/composition/InstallSpatial3DSystems.hpp>

#include <lux/engine/ecs/scene_format/spatial3d/SceneCatalog.hpp>
#include <lux/engine/ecs/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/ecs/spatial3d/streaming/Spatial3DSectionSource.hpp>
#include <lux/engine/ecs/spatial3d/streaming/SpatialInterest3DSystem.hpp>
#include <lux/engine/ecs/entity_scene/residency/SectionResidencyPlanner.hpp>
#include <lux/engine/ecs/entity_scene/residency/EntitySectionResidencySystem.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        [[nodiscard]] bool recordHasChannel(
            const lux::ecs::scene_format::SectionRecord& record,
            const lux::ecs::scene_format::DemandChannelId& channel) noexcept
        {
            return std::any_of(
                record.demand_channels.begin(),
                record.demand_channels.end(),
                [&channel](const auto& candidate)
                {
                    return candidate.view() == channel.view();
                });
        }

        [[nodiscard]] const lux::ecs::scene_format::SectionRecord*
        findSection(
            std::span<const lux::ecs::scene_format::SectionRecord> records,
            lux::ecs::scene_format::EntitySectionId id) noexcept
        {
            const auto found = std::lower_bound(
                records.begin(),
                records.end(),
                id,
                [](const auto& record, const auto& target)
                {
                    return record.id.value() < target.value();
                });
            return found != records.end() && found->id == id
                ? &*found
                : nullptr;
        }

        struct ValidatedCatalog final
        {
            lux::ecs::entity_scene::residency::SectionResidencyBudget budget;
            lux::ecs::spatial3d::streaming::SpatialInterest3DConfig interest;
        };

        [[nodiscard]] std::optional<ValidatedCatalog>
        validateAndBuildCatalog(
            const lux::scene::SceneDescription& scene,
            lux::ecs::scene_format::spatial3d::SceneCatalog config)
        {
            using lux::ecs::scene_format::spatial3d::SceneCatalogEntry;

            const lux::ecs::spatial3d::streaming::ResidencyCapacity capacity{
                config.residency.maximum_decoded_bytes,
                config.residency.maximum_entities,
                config.residency.maximum_interest_sources,
                config.residency.maximum_sections_per_interest};
            if (!capacity.valid())
                return std::nullopt;

            // The Engine-owned codec already guarantees canonical bands, one
            // entry per Section, one cell per band and at least one entry in
            // every band. Here the contribution additionally proves that the
            // opaque config is an exact index over the authoritative LXSC
            // records, rather than a second, drifting world catalog.
            std::map<uuids::uuid, std::uint32_t> configured_sections;
            std::vector<lux::ecs::scene_format::DemandChannelId>
                catalog_channels;
            for (const auto& band : config.bands)
            {
                if (std::none_of(
                        catalog_channels.begin(),
                        catalog_channels.end(),
                        [&band](const auto& channel)
                        {
                            return channel.view() ==
                                band.demand_channel.view();
                        }))
                {
                    catalog_channels.push_back(band.demand_channel);
                }
            }
            for (const auto& entry : config.entries)
            {
                const auto* const record = findSection(
                    scene.sections,
                    entry.section);
                if (!record || entry.band >= config.bands.size() ||
                    !recordHasChannel(
                        *record,
                        config.bands[entry.band].demand_channel))
                {
                    return std::nullopt;
                }
                if (!configured_sections.emplace(
                        entry.section.value(), entry.band).second)
                {
                    return std::nullopt;
                }
            }

            for (const auto& record : scene.sections)
            {
                std::optional<lux::ecs::scene_format::DemandChannelId>
                    record_catalog_channel;
                for (const auto& channel : catalog_channels)
                {
                    if (!recordHasChannel(record, channel))
                    {
                        continue;
                    }
                    if (record_catalog_channel)
                        return std::nullopt;
                    record_catalog_channel = channel;
                }
                const auto configured = configured_sections.find(
                    record.id.value());
                if (record_catalog_channel)
                {
                    if (configured == configured_sections.end())
                    {
                        return std::nullopt;
                    }
                    const auto& assigned =
                        config.bands[configured->second].demand_channel;
                    if (assigned.view() !=
                        record_catalog_channel->view())
                    {
                        return std::nullopt;
                    }
                }
                else if (configured != configured_sections.end())
                {
                    return std::nullopt;
                }
            }

            lux::ecs::spatial3d::streaming::SpatialInterest3DConfig interest;
            interest.maximum_sources =
                capacity.maximum_interest_sources;
            interest.bands.reserve(config.bands.size());
            const auto maximum_sections_per_source =
                capacity.maximum_sections_per_interest;
            std::set<std::string> source_names;
            for (std::uint32_t band_index = 0u;
                 band_index < config.bands.size(); ++band_index)
            {
                const auto& cooked_band = config.bands[band_index];
                auto source_namespace =
                    lux::ecs::spatial3d::streaming::demandSourceNamespace(
                        cooked_band);
                if (!source_names.insert(
                        std::string{source_namespace.name()}).second)
                    return std::nullopt;
                std::vector<lux::ecs::spatial3d::streaming::
                    Spatial3DSectionCatalogEntry> entries;
                for (const SceneCatalogEntry& entry : config.entries)
                {
                    if (entry.band == band_index)
                    {
                        entries.push_back({entry.coordinate, entry.section});
                    }
                }
                auto catalog = lux::ecs::spatial3d::streaming::
                    Spatial3DSectionCatalog::create(
                        std::move(entries));
                if (!catalog)
                    return std::nullopt;

                interest.bands.push_back(
                    lux::ecs::spatial3d::streaming::SpatialInterest3DBand{
                        .source_namespace =
                            std::move(source_namespace),
                        .sections = lux::ecs::spatial3d::streaming::
                            Spatial3DSectionSource::catalog(
                                std::move(*catalog)),
                        .cell_world_size = cooked_band.cell_world_size,
                        .channel = cooked_band.demand_channel,
                        .active_distance_scale =
                            cooked_band.active_distance_scale,
                        .resident_distance_scale =
                            cooked_band.resident_distance_scale,
                        .maximum_sections_per_source =
                            maximum_sections_per_source});
            }
            if (!interest.valid())
                return std::nullopt;

            return ValidatedCatalog{
                lux::ecs::entity_scene::residency::SectionResidencyBudget{
                    capacity.maximum_decoded_bytes,
                    capacity.maximum_entities},
                std::move(interest)};
        }
    }

    bool installSpatial3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        const lux::scene::SceneDescription& description)
    {
        if (description.spatial3d_catalog.empty())
            return true;

        constexpr std::string_view required_components[]{
            "lux.ecs.spatialinterest3dcomponent"};
        if (auto validated = lux::ecs::validateComponentSchemas(
                components, required_components); !validated)
        {
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto decoded = lux::ecs::scene_format::spatial3d::decodeSceneCatalog(
            description.spatial3d_catalog);
        auto* const sections = builder.services().borrow<
            lux::ecs::entity_scene::EntitySectionClient>();
        if (!decoded || !sections)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto validated = validateAndBuildCatalog(
            description,
            std::move(*decoded));
        if (!validated)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto planner = lux::ecs::entity_scene::residency::SectionResidencyPlanner::create(
            description.sections,
            validated->budget);
        if (!planner)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto partition = std::make_unique<
            lux::ecs::entity_scene::residency::EntitySectionResidencySystem>(
                *sections,
                std::move(*planner));
        auto* const partition_owner = partition.get();
        auto interest = std::make_unique<
            lux::ecs::spatial3d::streaming::SpatialInterest3DSystem>(
            *partition_owner,
            std::move(validated->interest));
        auto* const interest_owner = interest.get();
        if (!builder.add(std::move(partition)) ||
            !builder.services().adopt(*partition_owner) ||
            !builder.add(std::move(interest)) ||
            !builder.services().adopt(*interest_owner))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}
