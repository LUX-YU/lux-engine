#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <vector>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor::sessions
{
    class SessionOpening;
}
namespace lux::editor::persistence
{
    class IArtifactStore;
    class WriteCoordinator;
} // namespace lux::editor::persistence

namespace lux::editor
{
    class ProjectStorage;
    struct ProjectReloadReport final
    {
        sessions::ContentStamp source;
        std::optional<sessions::SessionFactoryResult<sessions::ContentStamp>> result;
    };

    // Owns admitted reload operations and their results independently of observing windows.
    // The original Session operation keeps the gate, code pin and file-version observation.
    class ProjectContentReloading final
    {
    public:
        // Shared providers are required; the project, runtime and scope outlive this activity.
        ProjectContentReloading(
            std::shared_ptr<sessions::SessionStore>,
            std::shared_ptr<sessions::SessionOpening>,
            ProjectStorage&,
            std::shared_ptr<persistence::WriteCoordinator>,
            std::shared_ptr<persistence::IArtifactStore>,
            process::ExecutionRuntime&,
            services::ServiceRegistry&,
            services::ServiceScope&
        );
        ~ProjectContentReloading();
        ProjectContentReloading(const ProjectContentReloading&) = delete;
        ProjectContentReloading& operator=(const ProjectContentReloading&) = delete;
        ProjectContentReloading(ProjectContentReloading&&) = delete;
        ProjectContentReloading& operator=(ProjectContentReloading&&) = delete;

        [[nodiscard]] EditorResult<void> request(sessions::ContentStamp);
        [[nodiscard]] EditorResult<void> update();
        [[nodiscard]] EditorResult<void> acknowledge(sessions::ContentStamp);
        [[nodiscard]] EditorResult<void> requestClose() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] EditorResult<std::vector<ProjectReloadReport>> reports() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    extern const services::ServiceDescriptor kProjectContentReloadingService;
} // namespace lux::editor
