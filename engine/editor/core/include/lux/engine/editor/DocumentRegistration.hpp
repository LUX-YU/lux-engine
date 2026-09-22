#pragma once

#include <lux/engine/editor/DocumentEditor.hpp>

#include <functional>
#include <string>

namespace lux::editor
{
class Project;

// A business package supplies an opening operation, not access to the
// product root. The application adopts these values during cold assembly.
struct DocumentRegistration final
{
    std::string type;
    std::function<EditorResult<std::unique_ptr<DocumentOpening>>(Project &, const OpenDocumentRequest &)> open;
};
} // namespace lux::editor
