#include <lux/engine/runtime/spatial3d/partitioned/InstallSpatial3DStreamingSystems.hpp>

#include <lux/engine/spatial3d/SceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/ecs/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DSectionSource.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/SpatialInterest3DSystem.hpp>
#include <lux/engine/ecs/entity_scene/residency/EntitySectionRecordStore.hpp>
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
    lux::ecs::entity_scene::residency::SectionDemandSourceId
    spatial3DDemandSourceNamespace(
        const lux::spatial3d::SceneCatalogBand& band)
    {
        // Length-prefix both canonical identities.  Dot concatenation alone
        // is ambiguous because dots are legal inside either operand.
        const auto source = band.source.name();
        const auto channel = band.demand_channel.name();
        auto name = std::string{"lux.spatial3d.band."} +
            std::to_string(source.size()) + ".";
        name.append(source);
        name += "." + std::to_string(channel.size()) + ".";
        name.append(channel);
        name += ".l" + std::to_string(band.level);
        return lux::ecs::entity_scene::residency::SectionDemandSourceId{
            std::move(name)};
    }

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

        struct ValidatedCatalog final
        {
            lux::ecs::entity_scene::residency::SectionResidencyBudget budget;
            spatial3d::SpatialInterest3DConfig interest;
        };

        [[nodiscard]] std::optional<ValidatedCatalog>
        validateAndBuildCatalog(
            const entity_scene::EntitySceneCatalog& scene,
            lux::spatial3d::SceneCatalog config)
        {
            using lux::spatial3d::SceneCatalogEntry;

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
                const auto* const record = scene.findSection(entry.section);
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

            for (const auto& record : scene.sections())
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

            spatial3d::SpatialInterest3DConfig interest;
            interest.maximum_sources =
                config.residency.maximum_interest_sources;
            interest.bands.reserve(config.bands.size());
            std::set<std::string> source_names;
            for (std::uint32_t band_index = 0u;
                 band_index < config.bands.size(); ++band_index)
            {
                const auto& cooked_band = config.bands[band_index];
                auto source_namespace =
                    spatial3DDemandSourceNamespace(cooked_band);
                if (!source_names.insert(
                        std::string{source_namespace.name()}).second)
                    return std::nullopt;
                std::vector<spatial3d::Spatial3DSectionCatalogEntry> entries;
                for (const SceneCatalogEntry& entry : config.entries)
                {
                    if (entry.band == band_index)
                    {
                        entries.push_back({entry.coordinate, entry.section});
                    }
                }
                auto catalog = spatial3d::Spatial3DSectionCatalog::create(
                    std::move(entries));
                if (!catalog)
                    return std::nullopt;

                interest.bands.push_back(spatial3d::SpatialInterest3DBand{
                    .source_namespace =
                        std::move(source_namespace),
                    .sections = spatial3d::Spatial3DSectionSource::catalog(
                        std::move(*catalog)),
                    .cell_world_size = cooked_band.cell_world_size,
                    .channel = cooked_band.demand_channel,
                    .active_distance_scale =
                        cooked_band.active_distance_scale,
                    .resident_distance_scale =
                        cooked_band.resident_distance_scale,
                    .maximum_sections_per_source =
                        config.residency.maximum_sections_per_interest});
            }
            if (!interest.valid())
                return std::nullopt;

            return ValidatedCatalog{
                lux::ecs::entity_scene::residency::SectionResidencyBudget{
                    config.residency.maximum_decoded_bytes,
                    config.residency.maximum_entities},
                std::move(interest)};
        }
    }

    bool installSpatial3DStreamingSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components)
    {
        auto* const scene = builder.services().borrow<
            entity_scene::EntitySceneCatalog>();
        if (!scene)
            return false;
        if (scene->package().spatial3d_catalog.empty())
            return true;

        constexpr std::string_view required_components[]{
            "lux.ecs.spatialinterest3dcomponent"};
        if (auto validated = lux::ecs::validateComponentSchemas(
                components, required_components); !validated)
        {
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto decoded = lux::spatial3d::decodeSceneCatalog(
            scene->package().spatial3d_catalog);
        auto* const sections = builder.services().borrow<
            lux::ecs::entity_scene::EntitySectionClient>();
        if (!decoded || !sections)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto validated = validateAndBuildCatalog(*scene, std::move(*decoded));
        if (!validated)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto planner = lux::ecs::entity_scene::residency::SectionResidencyPlanner::create(
            lux::ecs::entity_scene::residency::EntitySectionRecordStore{
                scene->sections()},
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
        auto interest = std::make_unique<spatial3d::SpatialInterest3DSystem>(
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
