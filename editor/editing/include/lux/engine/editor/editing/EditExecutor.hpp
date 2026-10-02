#pragma once
#include <lux/engine/editor/editing/EditOperation.hpp>

namespace lux::editor::editing
{
    class EditHistory;

    // Stateless synchronous execution. Owner-thread and reentry protection belong to the log,
    // so a second executor cannot bypass an active prepare, publish or reclamation phase.
    class LUX_EDIT_HISTORY_PUBLIC EditExecutor final
    {
    public:
        // Failure preserves the input. CHANGE transfers it; NO_CHANGE destroys it under the
        // history gate without changing the current state, redo branch or observation counters.
        [[nodiscard]] EditResult<ApplyResult> execute(EditHistory&, EditOperationPtr&) const noexcept;
        [[nodiscard]] EditResult<ApplyResult> undo(EditHistory&) const noexcept;
        [[nodiscard]] EditResult<ApplyResult> redo(EditHistory&) const noexcept;
        [[nodiscard]] EditResult<void> clear(EditHistory&) const noexcept;
        // Explicit close notifies once. Destruction of EditHistory releases storage silently.
        [[nodiscard]] EditResult<void> close(EditHistory&) const noexcept;

    private:
        [[nodiscard]] EditResult<ApplyResult> replay(EditHistory&, EApplyKind) const noexcept;
    };
}
