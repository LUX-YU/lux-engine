#pragma once
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <lux/engine/editor/persistence/WriteLane.hpp>
#include <memory>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor::persistence
{
    class SaveExecution;
    class WriteCoordinator;
    class IArtifactStore;
}
namespace lux::editor
{
    // A project transaction publishes immutable files first and the captured manifest last.
    // It borrows the application's sole publication lane/execution owner. No private write queue.
    class ProjectPublicationOperation final
    {
    public:
        ProjectPublicationOperation(
            ProjectStorage&,
            process::ExecutionRuntime&,
            persistence::WriteCoordinator&,
            persistence::IArtifactStore&,
            persistence::SaveExecution&,
            ProjectPublication
        );
        ~ProjectPublicationOperation();
        ProjectPublicationOperation(const ProjectPublicationOperation&) = delete;
        ProjectPublicationOperation& operator=(const ProjectPublicationOperation&) = delete;
        ProjectPublicationOperation(ProjectPublicationOperation&&) = delete;
        ProjectPublicationOperation& operator=(ProjectPublicationOperation&&) = delete;
        void update();
        [[nodiscard]] const VPublicationStatus& status() const noexcept;
        [[nodiscard]] std::optional<persistence::WriteTicket> ticket() const noexcept;
        [[nodiscard]] bool terminal() const noexcept;
        [[nodiscard]] EditorResult<void> retry();
        void abandon();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
