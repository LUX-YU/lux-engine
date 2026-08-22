#include <lux/engine/runtime/scene/composition/InstallTilemapSystems.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapSystem.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapChunkSystem.hpp>

#include <memory>
#include <string_view>

namespace lux::runtime
{
    bool installTilemap2DSystems(
        lux::ecs::ScheduleBuilder& builder,
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
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto* const async = builder.services().borrow<SceneAsyncContext>();
        auto* const blobs = builder.services().borrow<
            entity_scene::ContentBlobClient>();
        auto* const persistent = builder.services().borrow<
            PersistentEntityIndex>();
        if (!async || !blobs || !persistent || !preparation)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto published_runtime =
            builder.services().emplace<TilemapRuntime>();
        if (!published_runtime)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto* const runtime_owner = *published_runtime;
        auto tilemaps = std::make_unique<TilemapSystem>(
            *runtime_owner,
            *persistent);
        auto* const tilemap_owner = tilemaps.get();
        const auto* activity = builder.services().get<
            spatial2d::TilemapChunkActivity2D>();
        if (!builder.add(std::move(tilemaps)) ||
            !builder.services().adopt(*tilemap_owner) ||
            !builder.add(
                std::make_unique<spatial2d::TilemapChunkSystem>(
                    async->runtime(),
                    async->scope(),
                    preparation,
                    *runtime_owner,
                    *tilemap_owner,
                    *blobs,
                    activity),
                kPhaseSceneLoading))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
} // namespace lux::runtime
