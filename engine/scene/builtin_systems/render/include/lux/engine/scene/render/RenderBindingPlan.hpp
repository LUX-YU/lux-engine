#pragma once

#include <lux/engine/scene/render/RenderBindingDeclarations.hpp>

namespace lux::scene
{
    // Cold, owned qualification for one selected capability. Contains no Registry or GPU objects.
    struct RenderBindingPlan final
    {
        RenderSystemConfiguration configuration;
        render::CapabilitySelection implementation;
        std::vector<render::RenderPayloadContract> contracts;
        std::vector<RenderBindingDeclaration> declarations;
        std::vector<system::SystemDataContract> data_inputs;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC exec::task<render::RenderResult<RenderBindingPlan>>
    prepareRenderBindings(
        engine::render::RenderRequester&,
        const RenderBindingDeclarations&,
        RenderSystemConfiguration
    ) noexcept;
} // namespace lux::scene
