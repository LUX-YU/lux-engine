#pragma once
#include <lux/engine/editor/desktop/DesktopError.hpp>
#include <lux/engine/scene/RenderResources.hpp>
namespace lux::window
{
    class LuxWindow;
}
namespace lux::editor::desktop
{
    [[nodiscard]] DesktopResult<lux::scene::NativeSurfaceOutput> windowOutput(window::LuxWindow&) noexcept;
}
