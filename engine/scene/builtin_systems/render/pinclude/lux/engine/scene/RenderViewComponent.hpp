#pragma once

#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>
#include <lux/engine/scene/SceneCamera.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

namespace lux::scene::detail
{
    // Only RenderSystem writes these derived bindings. No asynchronous work runs from destruction.
    struct RenderViewBinding final
    {
        CameraTargetId id;
        render::ViewId view;
        render::EffectId effect;
        render::AttachmentId attachment;
        CameraTarget adopted;
        CameraRecipeKey recipe;
        render::CameraInput parameters;
        render::RenderError error;
        std::vector<render::RasterGeometrySelection> groups;
        std::vector<render::SceneLightInput> light_inputs;
        std::uint32_t capacity{};
        bool parameters_changed = true;
        bool output_changed = true;
    };

    struct RenderViewComponent final
    {
        std::vector<RenderViewBinding> bindings;
    };

    // Exit-only transfer when ECS destroys a binding. Never used to locate an active output.
    struct RetiringView final
    {
        simulation::ecs::Entity camera;
        RenderViewBinding binding;
    };
} // namespace lux::scene::detail
