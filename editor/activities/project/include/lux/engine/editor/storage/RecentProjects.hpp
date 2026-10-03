#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/persistence/WriteLane.hpp>
#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

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
    // User-level project browsing data. The original coordinator owns disk publication;
    // this owner retains read results, bounded entries and the final publication observation.
    class RecentProjects final
    {
    public:
        RecentProjects(std::filesystem::path user_directory, std::filesystem::path current_project, process::ExecutionRuntime&, persistence::WriteCoordinator&, persistence::IArtifactStore&, persistence::SaveExecution&);
        ~RecentProjects();
        RecentProjects(const RecentProjects&) = delete;
        RecentProjects& operator=(const RecentProjects&) = delete;
        RecentProjects(RecentProjects&&) = delete;
        RecentProjects& operator=(RecentProjects&&) = delete;

        [[nodiscard]] EditorResult<void> refresh();
        [[nodiscard]] EditorResult<void> reconcile();
        // Suspending new work does not suppress an already accepted read or publication result.
        [[nodiscard]] EditorResult<void> update(bool allow_new_work = true);
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] std::span<const std::filesystem::path> entries() const noexcept;
        [[nodiscard]] std::optional<persistence::WriteTicket> ticket() const noexcept;
        [[nodiscard]] const persistence::VPublicationOutcome* publication() const noexcept;
        [[nodiscard]] const EditorFailure* failure() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
