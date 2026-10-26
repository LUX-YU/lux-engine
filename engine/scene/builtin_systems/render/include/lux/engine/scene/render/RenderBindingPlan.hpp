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

    // Resolve one factory's inputs without mutating the immutable selection Plan. View input
    // identities are supplied only after that actual View has been installed.
    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC render::RenderResult<std::vector<render::CapabilitiesInstalled::Input>>
    resolveRenderBindingInputs(
        const RenderBindingDeclaration&,
        std::span<const render::CapabilitiesInstalled::Input> scene_inputs,
        std::span<const render::CapabilitiesInstalled::Input> view_inputs,
        std::span<const render::CapabilitiesInstalled::Input> asset_inputs
    ) noexcept;

    // Select only producers used by this Plan before submitting a capability installation.
    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC render::RenderResult<std::vector<render::CapabilitiesInstalled::Input>>
    selectRenderAssetInputs(
        const RenderBindingPlan&,
        std::span<const render::CapabilitiesInstalled::Input> asset_inputs
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC exec::task<render::RenderResult<RenderBindingPlan>>
    prepareRenderBindings(
        engine::render::RenderRequester&,
        const RenderBindingDeclarations&,
        RenderSystemConfiguration
    ) noexcept;
} // namespace lux::scene
