#pragma once
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/ui/Command.hpp>

namespace lux::editor::editing
{
    // Query copies identity and availability only. Execute rejects a replaced history.
    struct HistoryCommand final
    {
        lux::ui::ECommandPhase phase{lux::ui::ECommandPhase::QUERY};
        HistoryId history;
        EHistoryAction action{EHistoryAction::UNDO};
        bool undo{}, redo{};
        lux::ui::ECommandDispatchResult result{lux::ui::ECommandDispatchResult::NOT_FOUND};
    };
}
