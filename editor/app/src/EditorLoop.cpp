#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/tasks/TaskMonitor.hpp>
#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <algorithm>
#include <cstdio>

namespace lux::editor
{
    void Editor::Impl::waitForWork()
    {
        auto& execution = engine->execution();
        const bool has_requests =
            !reviewing_exit_ && (exit_intent || !close_requests_.empty() || !asset_requests_.empty() ||
                                 !menu_requests_.empty() || workspace_intent_);
        const bool has_ready_work = execution.hasPendingWork() || messages->statistics().pending ||
                                    root->hasPendingChanges() || close_decisions_pending_ || has_requests;
        if (has_ready_work)
            return;
        const auto deadline = presentation->nextFrameTime();
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now)
            return;
        if (deadline == std::chrono::steady_clock::time_point::max())
            window::LuxWindow::waitEvents();
        else
            window::LuxWindow::waitEvents(std::chrono::duration<double>(deadline - now).count());
    }

    int Editor::Impl::exec()
    {
        while (!exit_requested_)
        {
            collectInput();
            const auto completed = engine->execution().collectCompletions();
            if (!completed)
                fail({EEditorError::EXECUTION_FAILURE, "editor.completions", 0, {}, completed.error()});
            if (exit_requested_)
                break;
            const auto tasks = engine->execution().dispatchTaskEvents();
            if (!tasks)
                fail({EEditorError::EXECUTION_FAILURE, "editor.tasks", 0, {}, tasks.error()});
            if (exit_requested_)
                break;
            detail::reportSignalDelivery(context->taskMonitor().dispatchChanges(), "tasks.changed");
            project_->dispatchEvents();
            static_cast<void>(messages->dispatchPending());
            root->applyPendingChanges();
            handleRequests();
            if (exit_requested_)
                break;
            const auto frame = presentation->frameInfo();
            auto* ui_draw_data = frame.display_size.width > 0 ? presentation->tryAcquireDrawData() : nullptr;
            const auto updated = root->update(frame, ui_draw_data);
            if (!updated)
                fail({EEditorError::FRONTEND_FAILURE, "editor.ui.update", 0, {}, updated.error()});
            root->applyPendingChanges();
            handleRequests();
            if (exit_requested_)
                break;
            const auto submitted = presentation->applySceneInput();
            if (!submitted)
                fail({EEditorError::FRONTEND_FAILURE, "editor.ui.input", 0, {}, submitted.error()});
            if (exit_requested_)
                break;
            const auto advanced = engine->sceneRuntime().driveFrame();
            if (!advanced)
                fail({EEditorError::EXECUTION_FAILURE, "editor.scenes", 0, {}, advanced.error()});
            else
                for (const auto& failure : *advanced)
                    if (failure.scene == presentation->sceneId())
                        fail({EEditorError::FRONTEND_FAILURE, "editor.ui.scene", 0, {}, failure});
            const auto status = engine->renderContext()->runtime().status();
            if (!status.error.ok())
                fail({EEditorError::FRONTEND_FAILURE, "editor.render.terminal", 0, {}, status.error});
            if (!exit_requested_)
                waitForWork();
        }
        // exec ends the loop; member destruction owns task/resource synchronization.
        // The UI tree and its services remain valid until Editor itself is destroyed.
        const int result = outcome_ ? 0 : 5;
        std::fprintf(stderr, "[editor.exit] event=loop_returned code=%d failure=%u\n", result, unsigned(!outcome_));
        return result;
    }
}
