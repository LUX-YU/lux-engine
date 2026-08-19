#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DPartitionedContribution.hpp>

#include <lux/engine/resource/spatial3d_scene/Spatial3DSceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DSectionSource.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/SpatialInterest3DSystem.hpp>
#include <lux/engine/runtime/spatial3d/transform/Spatial3DTransformContribution.hpp>
#include <lux/engine/runtime/spatial_partition/EntitySectionRecordStore.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialDemandPlanner.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

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
    lux::runtime::spatial_partition::SpatialDemandSourceId
    spatial3DDemandSourceNamespace(
        const lux::spatial3d_scene::Spatial3DSceneCatalogBand& band)
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
        return lux::runtime::spatial_partition::SpatialDemandSourceId{
            std::move(name)};
    }

    namespace
    {
        using BuildResult = lux::cxx::expected<void, SceneContributionBuildFailure>;

        [[nodiscard]] SceneContributionBuildFailure invalidConfig() noexcept
        {
            return {
                ESceneContributionBuildError::INVALID_CONFIG,
                lux::ecs::typeToken<
                    lux::spatial3d_scene::Spatial3DSceneCatalogConfig>()};
        }

        [[nodiscard]] bool recordHasChannel(
            const lux::scene::SectionRecord& record,
            const lux::scene::DemandChannelId& channel) noexcept
        {
            return std::any_of(
                record.demand_channels.begin(),
                record.demand_channels.end(),
                [&channel](const auto& candidate)
                {
                    return lux::extensions::sameStableId(
                        candidate.view(), channel.view());
                });
        }

        struct ValidatedCatalog final
        {
            spatial_partition::SpatialPartitionBudget budget;
            spatial3d::SpatialInterest3DConfig interest;
        };

        [[nodiscard]] lux::cxx::expected<
            ValidatedCatalog,
            SceneContributionBuildFailure>
        validateAndBuildCatalog(
            const entity_scene::EntitySceneCatalog& scene,
            lux::spatial3d_scene::Spatial3DSceneCatalogConfig config)
        {
            using lux::spatial3d_scene::Spatial3DSceneCatalogEntry;

            // The resource codec already guarantees canonical bands, one
            // entry per Section, one cell per band and at least one entry in
            // every band. Here the contribution additionally proves that the
            // opaque config is an exact index over the authoritative LXSC
            // records, rather than a second, drifting world catalog.
            std::map<uuids::uuid, std::uint32_t> configured_sections;
            std::vector<lux::scene::DemandChannelId>
                catalog_channels;
            for (const auto& band : config.bands)
            {
                if (std::none_of(
                        catalog_channels.begin(),
                        catalog_channels.end(),
                        [&band](const auto& channel)
                        {
                            return lux::extensions::sameStableId(
                                channel.view(),
                                band.demand_channel.view());
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
                    return lux::cxx::unexpected(invalidConfig());
                }
                if (!configured_sections.emplace(
                        entry.section.value(), entry.band).second)
                {
                    return lux::cxx::unexpected(invalidConfig());
                }
            }

            for (const auto& record : scene.sections())
            {
                std::optional<lux::scene::DemandChannelId>
                    record_catalog_channel;
                for (const auto& channel : catalog_channels)
                {
                    if (!recordHasChannel(record, channel))
                    {
                        continue;
                    }
                    if (record_catalog_channel)
                        return lux::cxx::unexpected(invalidConfig());
                    record_catalog_channel = channel;
                }
                const auto configured = configured_sections.find(
                    record.id.value());
                if (record_catalog_channel)
                {
                    if (configured == configured_sections.end())
                    {
                        return lux::cxx::unexpected(invalidConfig());
                    }
                    const auto& assigned =
                        config.bands[configured->second].demand_channel;
                    if (!lux::extensions::sameStableId(
                            assigned.view(),
                            record_catalog_channel->view()))
                    {
                        return lux::cxx::unexpected(invalidConfig());
                    }
                }
                else if (configured != configured_sections.end())
                {
                    return lux::cxx::unexpected(invalidConfig());
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
                    return lux::cxx::unexpected(invalidConfig());
                std::vector<spatial3d::Spatial3DSectionCatalogEntry> entries;
                for (const Spatial3DSceneCatalogEntry& entry : config.entries)
                {
                    if (entry.band == band_index)
                    {
                        entries.push_back({entry.coordinate, entry.section});
                    }
                }
                auto catalog = spatial3d::Spatial3DSectionCatalog::create(
                    std::move(entries));
                if (!catalog)
                    return lux::cxx::unexpected(invalidConfig());

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
                return lux::cxx::unexpected(invalidConfig());

            return ValidatedCatalog{
                spatial_partition::SpatialPartitionBudget{
                    config.residency.maximum_decoded_bytes,
                    config.residency.maximum_entities},
                std::move(interest)};
        }
    }

    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSpatial3DPartitionedContribution(
        const lux::ecs::ComponentTypeCatalog& components)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.spatialinterest3dcomponent"};
        if (auto validated = lux::ecs::validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{
                lux::spatial3d_scene::kSpatial3DContributionName}};
        descriptor.display_name = "Partitioned 3D spatial residency";
        descriptor.required_contributions.emplace_back(
            std::string{kSpatial3DTransformContributionName});
        descriptor.required_services = {
            lux::ecs::typeToken<entity_scene::EntitySceneCatalog>(),
            lux::ecs::typeToken<entity_scene::EntitySectionClient>()};
        descriptor.provided_services = {
            lux::ecs::typeToken<
                spatial_partition::SpatialPartitionSystem>(),
            lux::ecs::typeToken<spatial3d::SpatialInterest3DSystem>()};
        descriptor.config_schema_version =
            lux::spatial3d_scene::kSpatial3DSceneCatalogSchemaVersion;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig contribution) -> BuildResult
        {
            auto decoded = lux::spatial3d_scene::decodeSpatial3DSceneCatalog(
                contribution.bytes.view());
            if (!decoded)
                return lux::cxx::unexpected(invalidConfig());

            auto* const scene = builder.findService<
                entity_scene::EntitySceneCatalog>(context);
            auto* const sections = builder.findService<
                entity_scene::EntitySectionClient>(context);
            if (!scene || !sections)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::MISSING_SERVICE,
                        !scene
                            ? lux::ecs::typeToken<
                                  entity_scene::EntitySceneCatalog>()
                            : lux::ecs::typeToken<
                                  entity_scene::EntitySectionClient>()});
            }

            auto validated = validateAndBuildCatalog(
                *scene, std::move(*decoded));
            if (!validated)
                return lux::cxx::unexpected(validated.error());

            auto planner = spatial_partition::SpatialDemandPlanner::create(
                spatial_partition::EntitySectionRecordStore{*scene},
                validated->budget);
            if (!planner)
                return lux::cxx::unexpected(invalidConfig());

            auto partition = std::make_unique<
                spatial_partition::SpatialPartitionSystem>(
                    *sections, std::move(*planner));
            auto* const partition_owner = partition.get();
            if (auto added = builder.addServiceSystem(
                    std::move(partition)); !added)
            {
                return added;
            }
            return builder.addServiceSystem(
                std::make_unique<spatial3d::SpatialInterest3DSystem>(
                    *partition_owner,
                    std::move(validated->interest)));
        };
        return descriptor;
    }
}
