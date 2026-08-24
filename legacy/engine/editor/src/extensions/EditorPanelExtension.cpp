#include <lux/engine/editor/extensions/EditorPanelExtension.hpp>

namespace lux::extensions
{
    InstallEditorPanels makeEditorPanelInstallAdapter(
        lux::editor::EditorPanels& panels)
    {
        return [&panels](ExtensionModuleEntrypoints entrypoints)
            -> lux::cxx::expected<void, std::uint32_t>
        {
            if (!entrypoints.module || !entrypoints.editor_panels)
            {
                return lux::cxx::unexpected(static_cast<std::uint32_t>(
                    EExtensionRegistrationError::INVALID_DESCRIPTOR));
            }
            lux::editor::EditorPanelInstallContext context{
                entrypoints.module};
            const auto installed = entrypoints.editor_panels(context);
            if (!installed)
            {
                return lux::cxx::unexpected(
                    static_cast<std::uint32_t>(installed.error));
            }
            auto committed = std::move(context).commit(panels);
            if (!committed)
            {
                return lux::cxx::unexpected(
                    static_cast<std::uint32_t>(committed.error()));
            }
            return {};
        };
    }
}
