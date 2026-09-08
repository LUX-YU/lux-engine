#include <lux/engine/editor/EditHistoryController.hpp>
#include <utility>

namespace lux::editor
{
    EditHistoryController::EditHistoryController(
        object::ObjectDispatcherRef dispatcher, ActiveEditHistory& histories
    ) noexcept
        : Object(std::move(dispatcher)), histories_(&histories)
    {
    }
    EditHistoryController::~EditHistoryController() = default;

    void EditHistoryController::undo() noexcept
    {
        const auto target = histories_->activeTarget();
        if (!target)
        {
            return; // LuxObject entry points themselves belong to the UI owner.
        }
        if (auto result = histories_->undo(); !result)
        {
            notify<failed>(EditHistoryActionFailure{*target, editing::EHistoryAction::UNDO, result.error()});
        }
    }
    void EditHistoryController::redo() noexcept
    {
        const auto target = histories_->activeTarget();
        if (!target)
        {
            return;
        }
        if (auto result = histories_->redo(); !result)
        {
            notify<failed>(EditHistoryActionFailure{*target, editing::EHistoryAction::REDO, result.error()});
        }
    }
    bool EditHistoryController::canUndo() const noexcept
    {
        const auto view = histories_->view();
        return view && view->has_target && view->target.undo == editing::EHistoryActionAvailability::READY;
    }
    bool EditHistoryController::canRedo() const noexcept
    {
        const auto view = histories_->view();
        return view && view->has_target && view->target.redo == editing::EHistoryActionAvailability::READY;
    }
    std::string_view EditHistoryController::undoLabel() const noexcept
    {
        const auto view = histories_->view();
        return view && view->has_target ? view->target.undo_label : std::string_view{};
    }
    std::string_view EditHistoryController::redoLabel() const noexcept
    {
        const auto view = histories_->view();
        return view && view->has_target ? view->target.redo_label : std::string_view{};
    }
} // namespace lux::editor
