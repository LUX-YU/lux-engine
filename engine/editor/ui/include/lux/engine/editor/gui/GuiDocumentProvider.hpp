#pragma once

#include <lux/engine/editor/DocumentRegistration.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/ui/UIRenderSystem.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>

namespace lux::editor::gui
{
struct GuiDocumentProvider final
{
    std::string type;
    std::function<bool(const ProjectAssetEntry &)> accepts;
    std::function<EditorResult<DocumentRegistration>(process::ExecutionRuntime &, render::RenderRuntime &)>
        registration;
    // A value registration has no dependency on the product's Editor root.
    // Concrete documents own their panes; the UI system only borrows them.
    std::function<EditorResult<void>(DocumentEditor &, ui::UIRenderSystem &, render::RenderRuntime &,
                                     process::ExecutionRuntime &)>
        attach;
};
} // namespace lux::editor::gui
