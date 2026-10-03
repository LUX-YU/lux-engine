#pragma once
#include <lux/engine/editor/workspace/LayoutCatalog.hpp>
#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <lux/engine/editor/workspace/LegacyWorkspaceMigration.hpp>
#include <lux/engine/editor/workspace/SettingsDocument.hpp>
#include <filesystem>

namespace lux::asset { class AssetId; }

namespace lux::editor::workspace
{
    struct StoredLayout final
    {
        DockLayout value;
        persistence::WriteTarget target;
    };
    struct StoredPreferences final
    {
        UserPreferences value;
        persistence::WriteTarget target;
    };
    struct StoredRecovery final
    {
        RecoveryManifest value;
        persistence::WriteTarget target;
    };
    struct LayoutChoice final
    {
        std::optional<DockLayout> layout;
        std::optional<WorkspaceFailure> fallback_reason;
    };
    class WorkspaceStore final
    {
    public:
        // project_root already exists. Both dependencies belong to the application's P05 publication owner.
        WorkspaceStore(
            std::filesystem::path project_root,
            persistence::WriteCoordinator&,
            persistence::IArtifactStore&,
            WorkspaceLimits = {}
        );
        WorkspaceStore(const WorkspaceStore&) = delete;
        WorkspaceStore& operator=(const WorkspaceStore&) = delete;
        WorkspaceStore(WorkspaceStore&&) = delete;
        WorkspaceStore& operator=(WorkspaceStore&&) = delete;
        [[nodiscard]] WorkspaceResult<StoredLayout> readLayout(const LayoutId&) const;
        [[nodiscard]] WorkspaceResult<StoredPreferences> readPreferences() const;
        [[nodiscard]] WorkspaceResult<StoredRecovery> readRecovery() const;
        // Relative to this store's explicit physical root (project or user). Missing is distinct from IO.
        [[nodiscard]] WorkspaceResult<settings::SettingsDocument>
        readSettings(std::string_view relative, settings::ESettingsScope) const;
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket>
        writeSettings(std::string_view relative, const settings::SettingsDocument&);
        [[nodiscard]] WorkspaceResult<LayoutCatalog> listLayouts() const;
        // Defaults are read decisions only; access failures are returned, never persisted as defaults.
        [[nodiscard]] WorkspaceResult<LayoutChoice> chooseLayout(const UserPreferences&) const;
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket> saveLayout(
            const DockLayout&,
            std::string expected_version
        );
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket> renameLayout(const LayoutId&, std::string label);
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket> removeLayout(const LayoutId&);
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket> writePreferences(
            const UserPreferences&,
            std::string expected_version
        );
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket> writeRecovery(
            const RecoveryManifest&,
            std::string expected_version
        );
        [[nodiscard]] WorkspaceResult<LegacyMigration> prepareLegacyMigration() const;
        // Returns at most one accepted write. The caller settles/acknowledges it through P05 before retrying.
        // Empty means all records were verified and the marker is already present. No private queue/pump.
        [[nodiscard]] WorkspaceResult<std::optional<persistence::WriteTicket>>
        continueMigration(const LegacyMigration&);
        // Copy the old preferences once to this explicit user-project root. Original layouts and
        // recovery remain in their source store; their stable identities are not interpreted here.
        // Settle and acknowledge each returned write through the original coordinator before retry.
        [[nodiscard]] WorkspaceResult<std::optional<persistence::WriteTicket>>
        continuePreferencesMigration(const WorkspaceStore& source, const asset::AssetId& project);

    private:
        [[nodiscard]] WorkspaceResult<LegacyWorkspaceInput> captureLegacyInput() const;
        struct ReadFile final
        {
            std::vector<std::byte> bytes;
            persistence::WriteTarget target;
        };
        [[nodiscard]] WorkspaceResult<ReadFile> read(std::string_view relative) const;
        [[nodiscard]] WorkspaceResult<persistence::WriteTarget> target(std::string_view relative) const;
        [[nodiscard]] WorkspaceResult<persistence::WriteTicket> write(
            std::string_view relative,
            std::string expected_version,
            std::vector<std::byte> bytes
        );
        std::filesystem::path root_;
        persistence::WriteCoordinator& coordinator_;
        persistence::IArtifactStore& artifacts_;
        WorkspaceLimits limits_;
    };
}
