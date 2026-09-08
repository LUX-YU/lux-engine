#pragma once
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/editor/editing/visibility.h>
namespace lux::editor::editing
{
    enum class EHistoryAction : std::uint8_t
    {
        UNDO,
        REDO
    };
    enum class EHistoryActionAvailability : std::uint8_t
    {
        READY,
        EMPTY,
        BUSY,
        BLOCKED,
        CLOSED
    };
    enum class EHistoryTargetOutcome : std::uint8_t
    {
        CONTENT_APPLIED,
        TRANSIENT_CANCELLED
    };
    struct HistoryTargetView final
    {
        HistorySnapshot history;
        EHistoryActionAvailability undo{EHistoryActionAvailability::EMPTY};
        EHistoryActionAvailability redo{EHistoryActionAvailability::EMPTY};
        std::string_view undo_label, redo_label;
    };
    struct HistoryTargetResult final
    {
        EHistoryTargetOutcome outcome{EHistoryTargetOutcome::CONTENT_APPLIED};
        ApplyResult content;
    };
    class LUX_EDITOR_EDITING_PUBLIC EditHistoryTarget
    {
    public:
        virtual ~EditHistoryTarget() noexcept;
        [[nodiscard]] virtual HistoryId historyId() const noexcept = 0;
        [[nodiscard]] virtual EditResult<HistoryTargetView> historyView() const noexcept = 0;
        [[nodiscard]] virtual EditResult<HistoryTargetResult> undo() noexcept = 0;
        [[nodiscard]] virtual EditResult<HistoryTargetResult> redo() noexcept = 0;
    };
} // namespace lux::editor::editing
