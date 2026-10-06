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
        bool refresh_catalog{true};
    };

    // Owns accepted workspace writes and their observations. Store/Coordinator remain the sole
    // file/version and publication authorities. Drive to settled before releasing this owner.
    class WorkspaceChanges final
    {
    public:
        WorkspaceChanges(
            WorkspaceStore&,
            persistence::WriteCoordinator&,
            persistence::IArtifactStore&,
            const WorkspaceStore* legacy_source = nullptr
        );
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
        // Accepted bytes/results outlive a settings page. This owner acknowledges the coordinator;
        // consumers acknowledge this report only after observing the actual publication outcome.
        [[nodiscard]] EditorResult<persistence::WriteTicket>
        saveSettings(std::string_view relative, const settings::SettingsDocument&);
        [[nodiscard]] EditorResult<void> migrate();
        [[nodiscard]] EditorResult<void> migrateProfile(const WorkspaceStore&, const asset::AssetId&);
        [[nodiscard]] bool migrationPending() const noexcept;
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
} // namespace lux::editor::workspace
