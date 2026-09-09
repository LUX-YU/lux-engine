#pragma once
#include <lux/engine/editor/ui/layout/WorkspaceLayout.hpp>
#include <array>
#include <string>
namespace lux::editor::ui
{
    struct SceneLayoutMetrics final
    {
        float outline_width{220}, inspector_width{340}, resources_height{210};
    };
    [[nodiscard]] inline WorkspaceLayout sceneLayout(WorkspaceId id, const std::array<std::string, 5> &panes,
                                                     SceneLayoutMetrics metrics = {})
    {
        lux::ui::SplitLayout split;
        split.center = panes[0];
        split.left = panes[1];
        split.right = panes[2];
        split.bottom = panes[3];
        split.toolbar = panes[4];
        split.left_width = metrics.outline_width;
        split.right_width = metrics.inspector_width;
        split.bottom_height = metrics.resources_height;
        return {id, 1, std::move(split)};
    }
} // namespace lux::editor::ui
