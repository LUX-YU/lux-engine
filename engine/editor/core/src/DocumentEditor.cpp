#include <lux/engine/editor/Editor.hpp>

namespace lux::editor
{
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
    EditorFrontend::~EditorFrontend() = default;
} // namespace lux::editor
