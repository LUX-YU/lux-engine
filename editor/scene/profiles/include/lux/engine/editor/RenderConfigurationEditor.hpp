#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>

namespace lux::editor
{
    class EditorComposition;
    // Adds the Scene Render configuration widget; choices come from the active Server capability catalog.
    [[nodiscard]] FrameworkResult<void> registerRenderConfigurationEditor(EditorComposition&) noexcept;
} // namespace lux::editor
