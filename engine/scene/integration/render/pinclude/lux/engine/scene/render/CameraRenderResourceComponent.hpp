#pragma once

#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/scene/render/CameraParameters.hpp>
#include <lux/engine/scene/render/CameraTargetsComponent.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

namespace lux::scene::detail
{
    // Only Camera bindings write these derived bindings. No asynchronous work runs from destruction.
    struct CameraRenderTarget final
    {
        CameraTargetId id;
        render::ViewId view;
        render::EffectId effect;
        render::AttachmentId attachment;
        CameraTarget adopted;
        CameraGraphInputs graph_inputs;
        render::CameraInput parameters;
        render::RenderError error;
        std::uint64_t input_revision{}, effect_revision{};
        bool parameters_changed = true;
        bool output_changed = true;
    };

    struct CameraRenderResourceComponent final
    {
        std::vector<CameraRenderTarget> bindings;
    };

    // Exit-only transfer when ECS destroys a binding. Never used to locate an active output.
    struct RetiringView final
    {
        simulation::ecs::Entity camera;
        CameraRenderTarget binding;
    };
} // namespace lux::scene::detail
