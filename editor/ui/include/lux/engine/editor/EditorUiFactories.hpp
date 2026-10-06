#pragma once
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class EditorContext;
    [[nodiscard]] FrameworkResult<std::unique_ptr<ui::Pane>>
    createPane(EditorContext&, const PaneDescription&) noexcept;
} // namespace lux::editor
