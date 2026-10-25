#pragma once
#include <exec/task.hpp>
#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/scene/RenderAssetInput.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Registry.hpp>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::engine::render
{
    class RenderRequester;
    class RenderAssets;
} // namespace lux::engine::render

namespace lux::scene
{
    struct RenderBindingPlan;

    struct RenderSystemDependencies final
    {
        engine::render::RenderRequester& client;
        execution::ExecutionRuntime& execution;
        engine::render::RenderAssets& assets;
        RenderAssetInput& assets_input;
        const RenderBindingPlan& plan;
        process::asset_loading::AssetReadPort source;
    };

    // Scene lifecycle and selected stages only. ECS/Render payload conversions live in registered bindings.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystem final
    {
    public:
        inline static constexpr std::string_view Capabilities[]{"lux.scene.optional_presentation"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.render",
            .version = 2,
            .configuration_schema_name = "lux.scene.render.Configuration",
            .configuration_schema_version = 2,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER
        };
        RenderSystem(simulation::ecs::Registry&, const RenderSystemDependencies&);
        ~RenderSystem() noexcept;
        RenderSystem(const RenderSystem&) = delete;
        RenderSystem& operator=(const RenderSystem&) = delete;

        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult prepareInputs(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult publish(SceneStageContext&) noexcept;
        // Main completion after native scope installation and EnTT connection. Caller retains the Scene lease.
        [[nodiscard]] exec::task<render::RenderResult<void>> whenReady() noexcept;

        // Cold non-owning query. A missing/ambiguous role returns null; this never constructs a behavior.
        template <class T> [[nodiscard]] T* binding(std::string_view role = {}) noexcept
        {
            return static_cast<T*>(findBinding(cxx::typeToken<T>(), role));
        }

    private:
        void* findBinding(cxx::TypeToken, std::string_view) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC SceneSystemRegistration renderSystemRegistration() noexcept;
} // namespace lux::scene
