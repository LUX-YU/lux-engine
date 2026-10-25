#pragma once

#include <lux/engine/scene/render/RenderBindingDeclarations.hpp>

namespace lux::scene
{
    // Registers Engine ECS behaviors. It does not create resources, install Render Features,
    // or instantiate Scene objects. The declarations can be extended by the application.
    // Explicit names select Camera output for effects without a CameraInput payload; no implicit 3D fallback.
    [[nodiscard]] RenderBindingRegistrationResult registerBuiltinRenderBindings(
        RenderBindingDeclarations&,
        std::span<const std::string_view> camera_capabilities = {}
    ) noexcept;
} // namespace lux::scene
