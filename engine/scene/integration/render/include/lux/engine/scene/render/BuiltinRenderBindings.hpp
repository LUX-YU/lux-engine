#pragma once

#include <lux/engine/scene/render/RenderBindingDeclarations.hpp>

namespace lux::scene
{
    // Registers Engine ECS behaviors. It does not create resources, install Render Features,
    // or instantiate Scene objects. The declarations can be extended by the application.
    [[nodiscard]] RenderBindingRegistrationResult registerBuiltinRenderBindings(RenderBindingDeclarations&) noexcept;
} // namespace lux::scene
