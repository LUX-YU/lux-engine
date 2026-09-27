#pragma once
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/scene/RenderResources.hpp>
namespace lux::window
{
    class LuxWindow;
}
namespace lux::editor::ui
{
    [[nodiscard]] LUX_EDITOR_UI_PUBLIC EditorResult<lux::scene::NativeSurfaceOutput>
    windowOutput(window::LuxWindow&) noexcept;
}
