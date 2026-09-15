#include <lux/engine/editor/ui/SceneLayout.hpp>
#include <string>

namespace lux::editor::ui
{
    lux::ui::SplitLayout sceneLayout(std::string_view pane_prefix)
    {
        const std::string prefix{pane_prefix};
        return {prefix + "-outliner",
                prefix + "-view",
                prefix + "-inspector",
                prefix + "-resources",
                260,
                350,
                200,
                "project"};
    }
} // namespace lux::editor::ui
