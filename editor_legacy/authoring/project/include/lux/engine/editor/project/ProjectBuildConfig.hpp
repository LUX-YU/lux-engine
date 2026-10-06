#pragma once

#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <memory>
#include <optional>

namespace lux::scene
{
    struct ScenePackage;
}

namespace lux::editor
{
    struct ProjectBuildConfig final
    {
        struct InitialScene final
        {
            std::string source_path; // Relative to Content, independent of the asset's VFS location.
            std::string mount_path;  // Relative to /Project.
            std::shared_ptr<const lux::scene::ScenePackage> package;
        };
        asset::AssetId project_id;
        std::string name;
        std::vector<ProjectPluginEntry> plugins;
        std::optional<InitialScene> initial_scene;
    };
}
