#include <lux/engine/editor/detail/EditorTestAccess.hpp>
#include <lux/engine/editor/assets/AssetImporter.hpp>
namespace lux::editor
{
    namespace
    {
        thread_local bool fail_menu_connection{};
    }
    EditorResult<void> EditorTestAccess::restoreWorkspace(Editor& editor, const detail::WorkspaceData& data)
    {
        return editor.impl_->restoreWorkspace(data);
    }
    void EditorTestAccess::queryCommand(Editor& editor, lux::ui::Command& command)
    {
        editor.impl_->applicationCommand(command);
    }
    void EditorTestAccess::failNextMenuConnection() noexcept
    {
        fail_menu_connection = true;
    }
    object::LuxObject::ConnectResult EditorTestAccess::menuConnection(object::LuxObject::ConnectResult result) noexcept
    {
        if (std::exchange(fail_menu_connection, false))
            return lux::cxx::unexpected(object::EConnectError::CAPACITY_EXHAUSTED);
        return result;
    }
    ui::Presentation* EditorTestAccess::ui(Editor& editor) noexcept
    {
        return editor.impl_->presentation.get();
    }
    render::RenderRuntime& EditorTestAccess::renderer(Editor& editor) noexcept
    {
        return editor.impl_->engine->renderContext()->runtime();
    }
    void EditorTestAccess::turn(Editor& editor)
    {
        auto& state = *editor.impl_;
        state.collectInput();
        static_cast<void>(state.context->execution().collectCompletions());
        static_cast<void>(state.context->execution().dispatchTaskEvents());
        state.context->project().dispatchEvents();
        if (std::exchange(state.close_decisions_pending_, false))
            state.applyCloseDecisions();
        static_cast<void>(state.messages->dispatchPending());
        editor.applyPendingChanges();
        state.handleRequests();
        if (state.exit_requested_)
            return;
        const auto frame = state.presentation->frameInfo();
        auto* data = frame.display_size.width > 0 ? state.presentation->tryAcquireDrawData() : nullptr;
        const auto updated = editor.update(frame, data);
        if (!updated)
            state.fail({EEditorError::FRONTEND_FAILURE, "test.ui.update"});
        editor.applyPendingChanges();
        const auto submitted = state.presentation->applySceneInput();
        if (!submitted)
            state.fail({EEditorError::FRONTEND_FAILURE, "test.ui.input"});
        const auto advanced = state.engine->sceneRuntime().driveFrame();
        if (!advanced)
            state.fail({EEditorError::EXECUTION_FAILURE, "test.scenes"});
        state.handleRequests();
    }
    void EditorTestAccess::captureMenu(Editor& editor, lux::ui::Pane& pane)
    {
        lux::ui::MenuRequest request{lux::ui::EMenuAction::OPEN, &pane};
        editor.impl_->receiveMenu(request);
    }
    bool EditorTestAccess::validMenu(Editor& editor)
    {
        return editor.impl_->validMenuTarget(editor.impl_->menu_target_);
    }
    lux::ui::ECommandDispatchResult EditorTestAccess::executeMenu(Editor& editor, editing::EHistoryAction action)
    {
        if (!validMenu(editor))
            return lux::ui::ECommandDispatchResult::NOT_FOUND;
        lux::ui::Command request{
            lux::ui::CommandIdView{action == editing::EHistoryAction::UNDO ? "lux.edit.undo" : "lux.edit.redo"},
            lux::ui::ECommandPhase::EXECUTE
        };
        editor.impl_->dispatchCommand(editor.impl_->menu_target_, request);
        return request.result;
    }
}
