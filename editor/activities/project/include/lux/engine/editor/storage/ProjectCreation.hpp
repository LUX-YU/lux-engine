#pragma once

#include <lux/engine/editor/project/ProjectBuildConfig.hpp>
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>

namespace lux::editor
{
    struct ProjectCreationResult final
    {
        std::filesystem::path project_file;
        asset::AssetId project_id;
        std::optional<ProjectPublicationFailure> cleanup_warning;
    };

    namespace detail
    {
        [[nodiscard]] LUX_EDITOR_STORAGE_PUBLIC ProjectPublicationPlan::PrepareResult prepareProjectCreation(
            std::filesystem::path,
            ProjectBuildConfig,
            std::stop_token
        ) noexcept;
        [[nodiscard]] LUX_EDITOR_STORAGE_PUBLIC EditorResult<ProjectCreationResult> publishNewProject(
            std::shared_ptr<const ProjectPublicationPlan>,
            std::stop_token
        ) noexcept;
    }

    namespace detail
    {
        template <class Prepared>
        [[nodiscard]] auto publishPreparedProject(
            Prepared prepared,
            process::BlockingScheduler blocking,
            std::stop_token stop
        )
        {
            return stdexec::then(
                stdexec::continues_on(std::move(prepared), blocking),
                [stop](ProjectPublicationPlan::PrepareResult input) noexcept -> EditorResult<ProjectCreationResult> {
                    if (!input)
                        return lux::cxx::unexpected(std::move(input.error()));
                    return publishNewProject(std::move(*input), stop);
                }
            );
        }
    }

    // No owner-thread hop. A durable commit is a value even if cancellation arrives afterwards.
    [[nodiscard]] inline auto createProject(
        process::ExecutionRuntime& execution,
        process::BlockingScheduler blocking,
        std::filesystem::path directory,
        ProjectBuildConfig config,
        std::stop_token stop = {}
    )
    {
        auto prepared = stdexec::then(
            stdexec::schedule(execution.cpu()),
            [directory = std::move(directory), config = std::move(config), stop]() mutable noexcept {
                return detail::prepareProjectCreation(std::move(directory), std::move(config), stop);
            }
        );
        return detail::publishPreparedProject(std::move(prepared), blocking, stop);
    }
}
