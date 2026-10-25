#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>

namespace lux::editor
{
    class EditorComposition;
    // Product assembly for the built-in author components; the Inspector contains no concrete type list.
    [[nodiscard]] FrameworkResult<void> registerComponentEditors3D(EditorComposition&) noexcept;
} // namespace lux::editor
