#pragma once
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/project/PluginManager.hpp>

namespace lux::editor
{
    // Blocking project-open preparation. Only declared catalogs inside this project may extend the
    // installation catalog. Returned runtime libraries own code independently of the calling task.
    [[nodiscard]] lux::project::PluginResult<lux::project::PluginManager> loadProjectPlugins(
        const std::filesystem::path& project_root,
        std::span<const ProjectPluginEntry> selection,
        const std::filesystem::path& installation
    );
}
