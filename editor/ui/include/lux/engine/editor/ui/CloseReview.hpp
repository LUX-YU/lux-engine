#pragma once

#include <lux/engine/editor/AssetEditing.hpp>
#include <lux/engine/editor/CloseRequest.hpp>
#include <lux/engine/editor/CloseStatus.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::ui
{
    // Concrete tools keep their save/model state. This only translates their retained result to a root event.
    template <class Tool>
    void reportCloseDecision(
        Tool& tool,
        CloseRequest& request,
        bool& prepared,
        std::optional<ECloseDecision>& reported
    ) noexcept
    {
        if (!request.id || request.action == ECloseAction::CANCEL)
            return;
        const auto deliver = [&](ECloseDecision state, std::optional<EditorFailure> failure = {}) {
            if (reported == state)
                return;
            reported = state;
            CloseDecision result{request.id, &tool, state, std::move(failure)};
            static_cast<void>(object::routeEvent(tool, tool.root(), result));
        };
        if (!prepared)
        {
            auto result = tool.prepareExit();
            if (!result)
            {
                if (result.error().code != EEditorError::BUSY)
                    deliver(ECloseDecision::FAILED, result.error());
                return;
            }
            prepared = true;
        }
        const auto& status = tool.assetStatus();
        if (status.failure)
            deliver(ECloseDecision::FAILED, status.failure);
        else if (status.change != EAssetChange::EXIT || status.phase == EAssetEditPhase::IDLE)
            deliver(ECloseDecision::CANCELLED);
        else if (status.phase == EAssetEditPhase::EXIT_READY)
            deliver(ECloseDecision::READY);
    }

    template <class Tool>
    bool receiveCloseRequest(
        Tool& tool,
        object::EventView& event,
        CloseRequest& retained,
        bool& prepared,
        std::optional<ECloseDecision>& reported
    ) noexcept
    {
        const auto* request = event.template getIf<CloseRequest>();
        if (!request)
            return false;
        event.accept();
        if (!request->id || request->id < retained.id)
            return true;
        if (request->action == ECloseAction::REVIEW)
        {
            if (request->id == retained.id)
                return true;
            retained = *request;
            prepared = false;
            reported.reset();
        }
        else if (request->action == ECloseAction::CANCEL && request->id == retained.id)
        {
            tool.cancelExit();
            retained.action = ECloseAction::CANCEL;
            reported.reset();
        }
        return true;
    }
}
