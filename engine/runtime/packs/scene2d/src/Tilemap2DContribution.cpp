#include <lux/engine/runtime/packs/spatial2d/Tilemap2DContribution.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapSystem.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapChunkSystem.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeTilemap2DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        spatial2d::TilemapPrepareClient preparation)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.tilemapcomponent",
            "lux.ecs.tilechunk2dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kTilemap2DContributionName}};
        descriptor.display_name = "2D tilemap content";
        descriptor.required_services = {
            lux::cxx::typeToken<SceneAsyncContext>(),
            lux::cxx::typeToken<entity_scene::ContentBlobClient>(),
            lux::cxx::typeToken<PersistentEntityIndex>()};
        descriptor.provided_services = {
            lux::cxx::typeToken<TilemapRuntime>(),
            lux::cxx::typeToken<TilemapSystem>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [preparation](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            auto* const async = builder.findService<SceneAsyncContext>(
                context);
            auto* const blobs = builder.findService<
                entity_scene::ContentBlobClient>(context);
            auto* const persistent = builder.findService<
                PersistentEntityIndex>(context);
            if (!async || !blobs || !persistent || !preparation)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        !async || !blobs || !persistent
                            ? ESceneContributionBuildError::MISSING_SERVICE
                            : ESceneContributionBuildError::BUILD_REJECTED,
                        !async
                            ? lux::cxx::typeToken<SceneAsyncContext>()
                            : !blobs
                                  ? lux::cxx::typeToken<
                                        entity_scene::ContentBlobClient>()
                                  : !persistent
                                        ? lux::cxx::typeToken<
                                              PersistentEntityIndex>()
                                        : lux::cxx::TypeToken{}});
            }

            auto published_runtime = builder.publishServiceAndGet(
                std::make_unique<TilemapRuntime>());
            if (!published_runtime)
            {
                return lux::cxx::unexpected(published_runtime.error());
            }
            auto* const runtime_owner = *published_runtime;
            auto tilemaps = std::make_unique<TilemapSystem>(
                *runtime_owner, *persistent);
            auto* const tilemap_owner = tilemaps.get();
            if (auto added = builder.addServiceSystem(
                    std::move(tilemaps)); !added)
            {
                return added;
            }
            const auto* activity = builder.findService<
                spatial2d::TilemapChunkActivity2D>(context);
            return builder.add(
                std::make_unique<spatial2d::TilemapChunkSystem>(
                    async->runtime(),
                    async->scope(),
                    preparation,
                    *runtime_owner,
                    *tilemap_owner,
                    *blobs,
                    activity),
                kPhaseSceneLoading);
        };
        return descriptor;
    }
} // namespace lux::runtime
