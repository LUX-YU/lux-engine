#include <lux/engine/editor/DocumentEditor.hpp>

namespace lux::editor
{
EditorResult<editing::HistorySnapshot> DocumentEditor::reviewClose() const
{
    if (closeStatus().state != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "document.close"});
    }
    const auto history = historyView();
    if (!history)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY,
                                                  "document.close.history",
                                                  static_cast<std::uint64_t>(history.error().code),
                                                  {},
                                                  history.error()});
    }
    using Availability = editing::EHistoryActionAvailability;
    if (history->undo == Availability::BUSY || history->redo == Availability::BUSY)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.close.history"});
    }
    for (const auto request : saveRequests())
    {
        const auto status = saveStatus(request);
        if (!status)
        {
            return lux::cxx::unexpected(status.error());
        }
        if (std::holds_alternative<SavePending>(*status) || std::holds_alternative<SaveRetryable>(*status))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.close.save"});
        }
    }
    return history->history;
}

EditorResult<SaveRequestId> DocumentEditor::requestSave(std::string)
{
    return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "document.save"});
}
std::span<const SaveRequestId> DocumentEditor::saveRequests() const noexcept
{
    return {};
}
EditorResult<SaveRequestStatus> DocumentEditor::saveStatus(SaveRequestId) const
{
    return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "document.save"});
}
EditorResult<void> DocumentEditor::retrySave(SaveRequestId)
{
    return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "document.save"});
}
EditorResult<void> DocumentEditor::abandonSave(SaveRequestId)
{
    return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "document.save"});
}
EditorResult<void> DocumentEditor::acknowledgeSave(SaveRequestId)
{
    return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "document.save"});
}
DocumentView::~DocumentView() = default;
DocumentEditor::~DocumentEditor() = default;
DocumentOpening::~DocumentOpening() = default;
} // namespace lux::editor
