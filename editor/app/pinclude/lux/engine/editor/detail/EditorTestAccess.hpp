#pragma once
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/editor/detail/EditorImpl.hpp>
namespace lux::editor
{
    struct EditorTestAccess final
    {
        LUX_EDITOR_APP_PUBLIC static void captureMenu(Editor&, lux::ui::Pane&);
        LUX_EDITOR_APP_PUBLIC static bool validMenu(Editor&);
        LUX_EDITOR_APP_PUBLIC static lux::ui::ECommandDispatchResult executeMenu(Editor&, editing::EHistoryAction);
        LUX_EDITOR_APP_PUBLIC static desktop::Presentation* ui(Editor&) noexcept;
        LUX_EDITOR_APP_PUBLIC static render::RenderRuntime& renderer(Editor&) noexcept;
        LUX_EDITOR_APP_PUBLIC static void turn(Editor&);
        LUX_EDITOR_APP_PUBLIC static EditorResult<void> restoreWorkspace(Editor&, const detail::WorkspaceData&);
        LUX_EDITOR_APP_PUBLIC static void queryCommand(Editor&, lux::ui::Command&);
        LUX_EDITOR_APP_PUBLIC static void failNextMenuConnection() noexcept;
        static object::LuxObject::ConnectResult menuConnection(object::LuxObject::ConnectResult) noexcept;
    };
}
