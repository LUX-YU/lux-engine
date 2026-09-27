#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/project/PluginLibrary.hpp>
#include <cassert>
#include <lux/engine/editor/scene/detail/SceneOpening.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>

namespace lux::editor::scene
{
    EditorResult<lux::scene::SceneInstanceId> detail::instantiateScenePackage(
        lux::scene::SceneRuntime& runtime,
        const lux::scene::ScenePackage& source,
        const SceneRegistrations& metadata,
        process::TaskScope& tasks,
        lux::render::RenderRuntime& renderer,
        lux::scene::RenderResources& resources,
        lux::scene::RenderAssetInput assets,

        bool open_all_partitions,
        lux::scene::FixedStepClock clock
    )
    {
        auto storage = process::world_loading::makeWorldMemoryStorageSource(
            std::shared_ptr<const lux::world::WorldDescription>(source.world, &source.world->data()),
            source.volumes
        );
        if (!storage)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.storage", 0, {}, storage.error()}
            );
        }
        lux::scene::WorldLoadingServices loading{std::move(*storage), tasks};
        if (open_all_partitions)
        {
            // This desktop opens the complete author document. Gameplay keeps
            // the captured description's explicit bootstrap/Observer demands.
            for (std::uint32_t ordinal{}; ordinal < source.world->data().partitionCount(); ++ordinal)
            {
                loading.bootstrap.push_back({ordinal});
            }
        }
        if (open_all_partitions)
            loading.initial_partitions = source.partitions;
        lux::scene::RenderFeatureSceneBindings render_bindings = metadata.render_bindings;
        std::array providers{
            lux::scene::makeSceneCapabilityProvider<lux::render::RenderRuntime>(
                "main-window",
                "lux.render.runtime",
                renderer
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::RenderFeatureSceneBindings>(
                "render-bindings",
                "lux.render.scene_bindings",
                render_bindings
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::RenderResources>(
                "resources",
                "lux.render.resources",
                resources
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::RenderAssetInput>(
                "assets",
                "lux.render.assets",
                assets
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::WorldLoadingServices>(
                "world-storage",
                "lux.world.loading",
                loading
            )
        };
        auto created =
            runtime.builder()
                .setDescription(std::shared_ptr<const lux::scene::SceneDescription>(source.scene, &source.scene->data())
                )
                .setWorld(std::shared_ptr<const lux::world::WorldDescription>(source.world, &source.world->data()))
                .setSimulation(std::shared_ptr<const lux::simulation::SimulationDescription>(
                    source.simulation,
                    &source.simulation->data()
                ))
                .setRegistrations(metadata.components, *metadata.simulation_systems, metadata.scene_systems)
                .setProviders(providers)
                .setClock(clock)
                .build();
        if (!created)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.create", 0, {}, created.error()}
            );
        }
        const auto stopped = runtime.invalid(*created);
        if (!stopped)
            std::terminate();
        auto& registry = runtime.getSceneRegistry(*created)->get();
        if (open_all_partitions && !registry.ctx().contains<lux::scene::WorldResidency>())
        {
            static_cast<void>(runtime.destroy(*created));
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.world-loading.required"});
        }
        return *created;
    }
}
