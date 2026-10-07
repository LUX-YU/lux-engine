#pragma once
#include <lux/engine/editor/SceneProfileRegistrar.hpp>

namespace lux::editor
{
    // Explicit product contribution; no implicit registration and no Editor camera/entities.
    [[nodiscard]] SceneProfileRegistration sceneProfile3D() noexcept;
} // namespace lux::editor
