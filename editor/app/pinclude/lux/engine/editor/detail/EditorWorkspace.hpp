#pragma once

#include <lux/engine/editor/WorkspaceRequest.hpp>
#include <lux/engine/editor/metadata/PaneState.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <filesystem>

namespace lux::editor::detail
{
    struct WorkspaceData final
    {
        std::vector<PaneState> panes;
        std::vector<std::pair<lux::ui::PaneId, bool>> child_visibility;
        lux::ui::DockState dock;
        std::vector<std::string> names;
        std::string selected;
    };
    [[nodiscard]] bool validLayoutName(std::string_view) noexcept;
    [[nodiscard]] EditorResult<WorkspaceData> readWorkspace(const std::filesystem::path&, std::string name);
    [[nodiscard]] EditorResult<WorkspaceData> writeWorkspace(
        const std::filesystem::path&,
        EWorkspaceAction,
        std::string name,
        std::string new_name,
        WorkspaceData
    );
}
