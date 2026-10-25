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

    // Pure cold qualification shared by capability UI and actual Scene preparation.
    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC render::RenderResult<RenderBindingPlan>
    resolveRenderBindings(const RenderBindingDeclarations&, const render::RenderCapability&) noexcept;

    // Qualifies role destinations after installation and before constructing any behavior.
    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC render::RenderResult<void> resolveRenderBindingInputs(
        RenderBindingPlan&,
        std::span<const render::CapabilitiesInstalled::Input> scene_inputs,
        std::span<const render::CapabilitiesInstalled::Input> resource_inputs
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC exec::task<render::RenderResult<RenderBindingPlan>>
    prepareRenderBindings(
        engine::render::RenderRequester&,
        const RenderBindingDeclarations&,
        RenderSystemConfiguration
    ) noexcept;
} // namespace lux::scene
