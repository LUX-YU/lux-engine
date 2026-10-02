#pragma once
#include <lux/engine/editor/project/ProjectCreationView.hpp>

namespace lux::editor::application
{
    // One bounded creation activity, shared by the Editor command and the standalone launcher.
    // No view or Root ownership; detached controls borrow this owner until the desktop retires them.
    class ProjectCreation final
    {
    public:
        ProjectCreation(
            process::ExecutionRuntime&,
            object::ObjectDispatcherRef,
            std::filesystem::path installation,
            bool launch_created = true
        );
        ~ProjectCreation() noexcept;
        ProjectCreation(const ProjectCreation&) = delete;
        ProjectCreation& operator=(const ProjectCreation&) = delete;
        ProjectCreation(ProjectCreation&&) = delete;
        ProjectCreation& operator=(ProjectCreation&&) = delete;
        [[nodiscard]] EditorResult<void> start();
        [[nodiscard]] project::ProjectCreationRequests requests();
        [[nodiscard]] const project::ProjectCreationProgress& progress() const noexcept;
        void update();
        void cancel() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
