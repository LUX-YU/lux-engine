#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <memory>

namespace lux::editor::persistence
{
    class IArtifactStore;
}
namespace lux::editor
{
    class ProjectStorage;
}
namespace lux::editor::workspace
{
    class WorkspaceStore;
    class WorkspaceChanges;
}
namespace lux::editor::extensions
{
    class ContributionRegistry;
    class ContributionSnapshot;
}
namespace lux::editor::application
{
    struct RestoredView final
    {
        workspace::RecoveryEntry entry;
        std::optional<sessions::OpenAssetId> opening;
        std::vector<sessions::OpenAssetStatus> sources;
        std::optional<EditorResult<views::ViewId>> result;
    };
    enum class ERestorationProgress : std::uint8_t { ACTIVE, SUSPENDED, CLOSING };

    // Product use case: content opening and exact view restoration. It borrows the existing owners,
    // holds immutable contributions across reads, and never inspects layout opaque data.
    class RestoreWorkbench final
    {
    public:
        using Present = cxx::function_ref<EditorResult<views::ViewId>(
            views::ViewContent, const extensions::ContributionSnapshot&, views::ViewRestoreKey, views::ViewTypeId
        )>;
        RestoreWorkbench(
            ProjectStorage&, persistence::IArtifactStore&, sessions::SessionStore&, sessions::SessionOpening&,
            workspace::WorkspaceStore&, workspace::WorkspaceChanges&, extensions::ContributionRegistry&
        );
        ~RestoreWorkbench();
        RestoreWorkbench(const RestoreWorkbench&) = delete;
        RestoreWorkbench& operator=(const RestoreWorkbench&) = delete;
        RestoreWorkbench(RestoreWorkbench&&) = delete;
        RestoreWorkbench& operator=(RestoreWorkbench&&) = delete;

        [[nodiscard]] EditorResult<void> capture(std::span<const views::ViewInfo>);
        [[nodiscard]] EditorResult<void> start();
        [[nodiscard]] EditorResult<void> update(ERestorationProgress, Present);
        [[nodiscard]] std::span<const RestoredView> items() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
