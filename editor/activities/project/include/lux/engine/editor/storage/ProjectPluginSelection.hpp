#pragma once

#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <memory>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor::persistence
{
    class WriteCoordinator;
    class IArtifactStore;
    class SaveExecution;
} // namespace lux::editor::persistence
namespace lux::editor
{
    // The manifest remains authoritative. This activity owns only the outstanding publication;
    // its successful result changes the next project-open selection, never the active plugin set.
    class ProjectPluginSelection final
    {
    public:
        ProjectPluginSelection(ProjectStorage&, process::ExecutionRuntime&, persistence::WriteCoordinator&, persistence::IArtifactStore&, persistence::SaveExecution&);
        ~ProjectPluginSelection();
        ProjectPluginSelection(const ProjectPluginSelection&) = delete;
        ProjectPluginSelection& operator=(const ProjectPluginSelection&) = delete;
        ProjectPluginSelection(ProjectPluginSelection&&) = delete;
        ProjectPluginSelection& operator=(ProjectPluginSelection&&) = delete;

        [[nodiscard]] EditorResult<void> request(
            std::span<const ProjectPluginEntry> based_on,
            std::vector<ProjectPluginEntry> desired
        );
        [[nodiscard]] EditorResult<void> retry();
        [[nodiscard]] EditorResult<void> abandon();
        [[nodiscard]] EditorResult<void> acknowledge();
        [[nodiscard]] EditorResult<void> update();
        // Owner-thread observation, valid until the next mutating call. Null means no publication.
        [[nodiscard]] const VPublicationStatus* status() const noexcept;
        [[nodiscard]] bool settled() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
