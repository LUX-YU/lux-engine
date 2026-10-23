#pragma once

#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Registry.hpp>

namespace lux::execution
{
    class TaskScope;
}

namespace lux::rendering
{
    class RenderExecution;
    class RenderAssets;
    struct FrameInput;
} // namespace lux::rendering

namespace lux::scene
{
    // Application-owned view input. The Scene appends to RenderRequester's open Main carrier;
    // it never submits a second frame or owns the window. Call beginFrame before driveFrame.
    struct SceneRenderInputs final
    {
        render::TargetId target;
        render::TargetDesc description;
        render::RasterViewport viewport;
        std::uint64_t revision = 0;       // Increment after changing target/viewport/exposure/background.
        std::uint64_t asset_revision = 0; // Project source generation; distinct generations may coexist.
        float exposure = 0;
        std::array<float, 4> background{0, 0, 0, 1};
    };

    struct SceneRenderServices final
    {
        rendering::RenderExecution& rendering;
        rendering::RenderAssets& assets;
        // Existing project cancellation/monitoring group. Retire Scene systems before closing its
        // admission; then settled() also proves their accepted protocol cleanup has returned.
        execution::TaskScope& tasks;
        SceneRenderInputs& inputs;
        // Optional project VFS; copied into each accepted load, never selected globally by workers.
        process::asset_loading::AssetReadPort source;
    };

    // Main-thread Scene projection. Observers collect intent; the existing TransformSystem remains
    // the sole world-matrix producer. Async work captures render data only, never the Registry.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystem final
    {
    public:
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.render",
            .version = 1,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .supported_world_types = SupportedWorldTypes
        };

        RenderSystem(simulation::ecs::Registry&, SceneRenderServices&);
        // Revokes future projection. Pending protocol results still settle, remove the Render Scene,
        // then return asset references on Main. Continue maintenance frames during project retirement.
        ~RenderSystem() noexcept;
        RenderSystem(const RenderSystem&) = delete;
        RenderSystem& operator=(const RenderSystem&) = delete;

        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult prepareInputs(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult publish(SceneStageContext&) noexcept;
        // Main-thread diagnostic/viewport association. Null until the candidate is actually attached;
        // this identity is a borrow, not a promise that a GPU frame has completed.
        [[nodiscard]] render::AttachmentId attachment() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC SceneSystemRegistration renderSystemRegistration() noexcept;
} // namespace lux::scene
