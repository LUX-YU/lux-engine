#pragma once
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>

namespace lux::editor::workspace
{
    struct RecoveryEntry final
    {
        views::ViewRestoreKey restore_key;
        views::ViewTypeId type;
        std::string locator;
        // A diagnostic fact: this manifest cannot recover changes that were never persisted.
        bool unpersisted_changes{};
    };
    struct RecoveryManifest final
    {
        std::uint32_t schema{1};
        std::vector<RecoveryEntry> entries;
        std::vector<PreservedOpaqueState> opaque;
        std::optional<LegacyOrigin> legacy_origin;
    };
    [[nodiscard]] WorkspaceResult<void> validateRecovery(const RecoveryManifest&, WorkspaceLimits = {});
    [[nodiscard]] WorkspaceResult<std::vector<std::byte>> encodeRecovery(const RecoveryManifest&, WorkspaceLimits = {});
    [[nodiscard]] WorkspaceResult<RecoveryManifest> decodeRecovery(std::span<const std::byte>, WorkspaceLimits = {});
}
