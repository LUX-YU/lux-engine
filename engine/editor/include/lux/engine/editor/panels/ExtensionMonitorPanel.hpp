#pragma once

#include <lux/engine/editor/visibility.h>
#include <lux/engine/ui/Panel.hpp>

#include <string>

namespace lux::extensions { class EngineExtensions; }
namespace lux::runtime { class SceneContributionCatalog; class RenderEffectCatalog; }
namespace lux::editor
{
    class EditorPanelCatalog;

    class LUX_EDITOR_PUBLIC ExtensionMonitorPanel final : public lux::ui::Panel
    {
    public:
        ExtensionMonitorPanel(
            std::string title,
            lux::extensions::EngineExtensions& extensions,
            const lux::runtime::SceneContributionCatalog& scene_contributions,
            const lux::runtime::RenderEffectCatalog& render_effects,
            const EditorPanelCatalog& editor_panels
        );

    private:
        void paint() override;

        lux::extensions::EngineExtensions* extensions_{nullptr};
        const lux::runtime::SceneContributionCatalog* scene_contributions_{nullptr};
        const lux::runtime::RenderEffectCatalog* render_effects_{nullptr};
        const EditorPanelCatalog* editor_panels_{nullptr};
    };
}
