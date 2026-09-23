#pragma once
#include <lux/engine/editor/metadata/visibility.h>
#include <memory>

namespace lux::editor
{
// Main-thread tool lifetime. The last editor/document lease releases the registry;
// late plugin registration uses drafts without reinitializing existing types.
[[nodiscard]] LUX_EDITOR_METADATA_PUBLIC std::shared_ptr<const void> acquireEditorReflection();
}
