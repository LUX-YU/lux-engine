#pragma once

#include <lux/engine/editor/project/ProjectBuildConfig.hpp>

namespace lux::editor
{
    enum class EProjectBuildError : std::uint8_t
    {
        MANIFEST,
        INVALID_SCENE,
        INVALID_PATH
    };
    struct ProjectBuildFailure final
    {
        EProjectBuildError code;
        ProjectManifestFailure manifest;
    };

    class LUX_EDITOR_PROJECT_PUBLIC ProjectBuilder final
    {
    public:
        ProjectBuilder(asset::AssetId id, std::string name) : value_{id, std::move(name)} {}
        void setName(std::string name) noexcept
        {
            value_.name = std::move(name);
        }
        void setPlugins(std::vector<ProjectPluginEntry> plugins) noexcept
        {
            value_.plugins = std::move(plugins);
        }
        void setInitialScene(ProjectBuildConfig::InitialScene value) noexcept
        {
            value_.initial_scene = std::move(value);
        }
        void clearInitialScene() noexcept
        {
            value_.initial_scene.reset();
        }
        using BuildResult = lux::cxx::expected<ProjectBuildConfig, ProjectBuildFailure>;
        [[nodiscard]] BuildResult build() && noexcept;

    private:
        ProjectBuildConfig value_;
    };
}
