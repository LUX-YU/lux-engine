#pragma once
#include <exec/task.hpp>

#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>
#include <lux/engine/scene/RenderAssetInput.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Registry.hpp>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::rendering
{
    class RenderRequester;
    class RenderAssets;
} // namespace lux::rendering

namespace lux::scene
{
    struct RenderSystemDependencies final
    {
        rendering::RenderRequester& client;
        execution::ExecutionRuntime& execution;
        rendering::RenderAssets& assets;
        RenderAssetInput& assets_input;
        // Optional project VFS; copied into each accepted load, never selected globally by workers.
        process::asset_loading::AssetReadPort source;
    };

    // Main-thread Scene projection. Observers collect intent; the existing TransformSystem remains
    // the sole world-matrix producer. Protocol continuations stay on Main; workers receive immutable inputs.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystem final
    {
    public:
        inline static constexpr system::SystemDataContract DataInputs[]{
            {"lux.ecs.Transform3D"},
            {"lux.ecs.Mesh3D", 2},
            {"lux.scene.Camera"},
            {"lux.transform.world3d.ready", 1, system::ESystemDataKind::PREPARED_DATA, system::ESystemDataPhase::STABLE}
        };
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.render",
            .version = 2,
            .configuration_schema_name = "lux.scene.render.Configuration",
            .configuration_schema_version = 1,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .data_inputs = DataInputs
        };

        RenderSystem(simulation::ecs::Registry&, const RenderSystemDependencies&, RenderSystemConfiguration = {});
        // Retire through maintain(context.stop): continue Main maintenance frames until COMPLETE.
        // Destruction never starts asynchronous work; Registry and dependencies remain alive until then.
        ~RenderSystem() noexcept;
        RenderSystem(const RenderSystem&) = delete;
        RenderSystem& operator=(const RenderSystem&) = delete;

        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult prepareInputs(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult publish(SceneStageContext&) noexcept;
        // Call in a Main Scene edit safe point. These functions patch only intent; maintain adopts it later.
        [[nodiscard]] render::RenderResult<CameraTargetId> addCameraTarget(
            simulation::ecs::Entity,
            CameraTarget
        ) noexcept;
        [[nodiscard]] bool updateCameraTarget(simulation::ecs::Entity, CameraTargetId, CameraTarget) noexcept;
        [[nodiscard]] bool removeCameraTarget(simulation::ecs::Entity, CameraTargetId) noexcept;
        // Main-thread diagnostic/viewport association. Null until the candidate is actually attached;
        // this identity is a borrow, not a promise that a GPU frame has completed.
        [[nodiscard]] render::AttachmentId attachment(simulation::ecs::Entity, CameraTargetId) const noexcept;
        [[nodiscard]] render::RenderError cameraTargetError(simulation::ecs::Entity, CameraTargetId) const noexcept;
        // A failed asset candidate does not remove the previously adopted instance binding.
        [[nodiscard]] render::RenderError assetError(simulation::ecs::Entity) const noexcept;
        // True only after an actual instance has adopted the current asset binding.
        [[nodiscard]] bool visualReady(simulation::ecs::Entity) const noexcept;
        // After removing the input, keep its Target alive until this returns true.
        [[nodiscard]] bool cameraTargetRetired(simulation::ecs::Entity, CameraTargetId) const noexcept;
        // Main completion for this exact ECS relation, never a global Scene transaction. The Scene/System
        // remains borrowed until completion; retired identity/error is returned explicitly.
        [[nodiscard]] exec::task<render::RenderResult<void>> whenCameraTargetReady(
            simulation::ecs::Entity,
            CameraTargetId
        ) noexcept;
        [[nodiscard]] exec::task<render::RenderResult<void>> whenCameraTargetRetired(
            simulation::ecs::Entity,
            CameraTargetId
        ) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC SceneSystemRegistration renderSystemRegistration() noexcept;
} // namespace lux::scene
