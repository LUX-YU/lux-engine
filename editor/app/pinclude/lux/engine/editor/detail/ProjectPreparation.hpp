#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/EditorConfig.hpp>
#include <lux/engine/editor/detail/PreparedProject.hpp>
#include <lux/engine/core/execution/Task.hpp>

namespace lux::editor::detail
{
    struct ProjectPreparation final
    {
        FrameworkResult<PreparedProject> result;
        bool published{};
    };
    [[nodiscard]] ProjectPreparation prepareProject(
        const std::filesystem::path&,
        const std::optional<ProjectManifest>& create,
        std::span<const ProjectPluginLocation>,
        execution::TaskReporter
    ) noexcept;
} // namespace lux::editor::detail
