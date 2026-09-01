#pragma once

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/render/visibility.h>

#include <array>
#include <memory>
#include <span>
#include <string_view>

namespace lux::scene
{
    class SceneBuilder;

    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystem final : public object::LuxObject
    {
    public:
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.render"}};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.builtin.system.render",
            .version = 1U,
            .configuration_schema_name = "lux.render.system.Configuration",
            .configuration_schema_version = 1U,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER
        };

        ~RenderSystem() noexcept override;

        [[nodiscard]] render::RenderSceneId renderSceneId() const noexcept;
        [[nodiscard]] bool publishStablePoint() noexcept;
        [[nodiscard]] bool presentationTick() noexcept;

    private:
        friend class SceneBuilder;

        RenderSystem(
            RenderRuntimeLease runtime,
            render::RenderSceneLease scene,
            std::unique_ptr<RenderSyncPipeline> sync
        ) noexcept;

        RenderRuntimeLease runtime_;
        render::RenderSceneLease scene_;
        std::unique_ptr<RenderSyncPipeline> sync_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC SceneSystemRegistration
    builtinRenderSystemRegistration() noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC std::span<const SceneSystemRegistration>
    builtinRenderSystemRegistrations() noexcept;

    LUX_ENGINE_SCENE_RENDER_PUBLIC void initializeBuiltinRenderSystemMeta() noexcept;
} // namespace lux::scene
