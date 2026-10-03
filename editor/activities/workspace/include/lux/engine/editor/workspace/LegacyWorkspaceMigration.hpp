#pragma once
#include <lux/engine/editor/workspace/LayoutCatalog.hpp>
#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>

namespace lux::editor::workspace
{
    struct LegacyWorkspaceFile final
    {
        std::string relative_path;
        std::vector<std::byte> bytes;
        std::string version;
    };
    struct LegacyWorkspaceInput final
    {
        std::vector<LegacyWorkspaceFile> layouts;
        std::optional<LegacyWorkspaceFile> settings;
        // Layouts are in canonical relative-path order after capture or conversion admission.
        [[nodiscard]] std::string sourceDigest() const;
    };
    class LegacyMigration final
    {
    public:
        [[nodiscard]] const std::vector<DockLayout>& layouts() const noexcept { return layouts_; }
        [[nodiscard]] const RecoveryManifest& recovery() const noexcept { return recovery_; }
        [[nodiscard]] const UserPreferences& preferences() const noexcept { return preferences_; }
        [[nodiscard]] const std::string& sourceDigest() const noexcept { return source_digest_; }
        [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }

    private:
        friend WorkspaceResult<LegacyMigration> prepareLegacyMigration(LegacyWorkspaceInput, WorkspaceLimits);
        LegacyMigration() = default;
        std::vector<DockLayout> layouts_;
        RecoveryManifest recovery_;
        UserPreferences preferences_;
        std::string source_digest_;
        std::vector<std::string> diagnostics_;
    };
    // Owned bytes only: no files, Root or content service are consulted by conversion.
    [[nodiscard]] WorkspaceResult<LegacyMigration>
    prepareLegacyMigration(LegacyWorkspaceInput, WorkspaceLimits = {});
}
