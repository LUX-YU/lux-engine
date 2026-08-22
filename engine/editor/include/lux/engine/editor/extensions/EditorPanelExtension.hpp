#pragma once
/**
 * @file EditorPanelExtension.hpp
 * @brief Direct editor-panel assembly adapter for Extension ABI v5.
 */

#include <lux/engine/editor/extensions/EditorPanels.hpp>
#include <lux/engine/editor/visibility.h>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>

namespace lux::extensions
{
    /// Invoke one module's editor entrypoint on the main thread and publish
    /// its UISystem registrations as one batch. The panel owner binds the
    /// module lease; no descriptor catalog is published.
    [[nodiscard]] LUX_EDITOR_PUBLIC InstallEditorPanels
    makeEditorPanelInstallAdapter(lux::editor::EditorPanels& panels);
}
