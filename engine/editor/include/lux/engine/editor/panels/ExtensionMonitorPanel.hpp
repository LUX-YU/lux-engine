#pragma once

#include <lux/engine/editor/visibility.h>
#include <lux/engine/ui/Panel.hpp>

#include <string>

namespace lux::extensions { class EngineExtensions; }
namespace lux::editor
{
    class EditorPanelCatalog;

    class LUX_EDITOR_PUBLIC ExtensionMonitorPanel final
        : public lux::ui::Panel
    {
    public:
        ExtensionMonitorPanel(
            std::string title,
            lux::extensions::EngineExtensions& extensions,
            const EditorPanelCatalog& editor_panels);

    private:
        void paint() override;

        lux::extensions::EngineExtensions* extensions_{nullptr};
        const EditorPanelCatalog* editor_panels_{nullptr};
    };
}
