#pragma once

#include <lux/engine/editor/ui/HistoryCommand.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::ui
{
    template <class Tool> void dispatchHistoryCommand(Tool& document, object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<editing::HistoryCommand>())
        {
            event.accept();
            const auto current = document.historyId();
            const auto view = document.historyView();
            request->undo = view && view->undo == editing::EHistoryActionAvailability::READY;
            request->redo = view && view->redo == editing::EHistoryActionAvailability::READY;
            if (request->phase == lux::ui::ECommandPhase::QUERY)
            {
                request->history = current;
                request->result = lux::ui::ECommandDispatchResult::DISABLED;
                return;
            }
            if (current != request->history)
                return;
            const auto finished = document.finishEditing();
            if (!finished || document.historyId() != request->history)
            {
                request->result = lux::ui::ECommandDispatchResult::FAILED;
                return;
            }
            const auto applied = request->action == editing::EHistoryAction::UNDO ? document.undo() : document.redo();
            request->result =
                applied ? lux::ui::ECommandDispatchResult::EXECUTED : lux::ui::ECommandDispatchResult::FAILED;
            return;
        }
        auto* command = event.getIf<lux::ui::Command>();
        if (!command)
            return;
        const bool undo = command->id == lux::ui::CommandIdView{"lux.edit.undo"};
        const bool redo = command->id == lux::ui::CommandIdView{"lux.edit.redo"};
        if (!undo && !redo)
            return;
        event.accept();
        const auto view = document.historyView();
        command->enabled = view &&
                           (undo ? view->undo : view->redo) == editing::EHistoryActionAvailability::READY;
        command->result = lux::ui::ECommandDispatchResult::DISABLED;
        if (command->phase != lux::ui::ECommandPhase::EXECUTE || !command->enabled)
            return;
        const auto finished = document.finishEditing();
        if (!finished)
        {
            command->result = lux::ui::ECommandDispatchResult::FAILED;
            return;
        }
        const auto applied = undo ? document.undo() : document.redo();
        command->result = applied ? lux::ui::ECommandDispatchResult::EXECUTED : lux::ui::ECommandDispatchResult::FAILED;
    }
}
