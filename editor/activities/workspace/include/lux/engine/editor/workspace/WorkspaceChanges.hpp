#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <memory>
#include <span>

namespace lux::editor::workspace
{
    struct WorkspacePublication final
    {
        std::string label;
        persistence::WriteTicket ticket;
        std::optional<persistence::VPublicationOutcome> result;
        std::optional<WorkspaceFailure> catalog_failure;
    };

    // Owns accepted workspace writes and their observations. Store/Coordinator remain the sole
    // file/version and publication authorities. Drive to settled before releasing this owner.
    class WorkspaceChanges final
    {
    public:
        WorkspaceChanges(WorkspaceStore&, persistence::WriteCoordinator&, persistence::IArtifactStore&);
        ~WorkspaceChanges();
        WorkspaceChanges(const WorkspaceChanges&) = delete;
        WorkspaceChanges& operator=(const WorkspaceChanges&) = delete;
        WorkspaceChanges(WorkspaceChanges&&) = delete;
        WorkspaceChanges& operator=(WorkspaceChanges&&) = delete;

        [[nodiscard]] EditorResult<void> refresh();
        [[nodiscard]] EditorResult<void> save(const DockLayout&);
        [[nodiscard]] EditorResult<void> rename(const LayoutId&, std::string label);
        [[nodiscard]] EditorResult<void> remove(const LayoutId&);
        // Call only after the Host has committed this layout; failure does not undo that UI fact.
        [[nodiscard]] EditorResult<void> select(const LayoutId&);
        [[nodiscard]] EditorResult<void> recordRecovery(const RecoveryManifest&, std::string version);
        [[nodiscard]] EditorResult<void> migrate();
        [[nodiscard]] EditorResult<void> reconcile(persistence::WriteTicket);
        [[nodiscard]] EditorResult<void> acknowledge(persistence::WriteTicket);
        [[nodiscard]] EditorResult<void> update(bool allow_new_work = true);
        [[nodiscard]] bool hasCapacity() const noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] const LayoutCatalog& catalog() const noexcept;
        [[nodiscard]] std::span<const WorkspacePublication> publications() const noexcept;
        [[nodiscard]] const LegacyMigration* migration() const noexcept;
        [[nodiscard]] const EditorFailure* migrationFailure() const noexcept;
        [[nodiscard]] bool migrationComplete() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
