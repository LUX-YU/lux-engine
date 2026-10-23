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
    class ExecutionRuntime;
}

namespace lux::rendering
{
    class RenderRequester;
    class RenderAssets;
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

    struct RenderSystemDependencies final
    {
        rendering::RenderRequester& client;
        execution::ExecutionRuntime& execution;
        rendering::RenderAssets& assets;
        SceneRenderInputs& inputs;
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
            {"lux.ecs.Mesh3D"},
            {"lux.scene.Camera"},
            {"lux.transform.world3d.ready", 1, system::ESystemDataKind::PREPARED_DATA, system::ESystemDataPhase::STABLE}
        };
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.render",
            .version = 1,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .data_inputs = DataInputs
        };

        RenderSystem(simulation::ecs::Registry&, const RenderSystemDependencies&);
        // Retire through maintain(context.stop): continue Main maintenance frames until COMPLETE.
        // Destruction never starts asynchronous work; Registry and dependencies remain alive until then.
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
