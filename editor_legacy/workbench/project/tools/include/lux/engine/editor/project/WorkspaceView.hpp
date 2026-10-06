#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <lux/engine/editor/workspace/LayoutCatalog.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::desktop
{
    struct UiDescriptor;
}

namespace lux::editor::project
{
    extern const desktop::UiDescriptor kWorkspaceView;
    struct RefreshWorkspace final
    {
    };
    struct SaveLayout final
    {
        std::string label;
    };
    struct ApplyLayout final
    {
        workspace::LayoutId layout;
    };
    struct RenameLayout final
    {
        workspace::LayoutId layout;
        std::string label;
    };
    struct RemoveLayout final
    {
        workspace::LayoutId layout;
    };
    struct AcknowledgeWorkspace final
    {
        persistence::WriteTicket ticket;
    };
    struct ReconcileWorkspace final
    {
        persistence::WriteTicket ticket;
    };
    struct CaptureRecovery final
    {
    };
    struct RestoreRecovery final
    {
    };
    struct MigrateWorkspace final
    {
    };
    using VWorkspaceIntent = std::variant<
        RefreshWorkspace,
        SaveLayout,
        ApplyLayout,
        RenameLayout,
        RemoveLayout,
        AcknowledgeWorkspace,
        ReconcileWorkspace,
        CaptureRecovery,
        RestoreRecovery,
        MigrateWorkspace>;

    struct WorkspacePublicationInfo final
    {
        std::string label;
        persistence::WriteTicket ticket;
        std::optional<persistence::VPublicationOutcome> result;
        std::optional<std::string> catalog_failure;
        bool unknown{};
    };
    struct WorkspaceSnapshot final
    {
        workspace::LayoutCatalog catalog;
        std::vector<std::string> diagnostics, recovery;
        std::vector<WorkspacePublicationInfo> publications;
    };

    // A presentation snapshot is not a second result owner. Requests carry their original
    // domain identities; the receiver revalidates and executes them at its safe point.
    class WorkspaceView final : public lux::ui::Pane
    {
    public:
        using Observe = cxx::move_only_function<EditorResult<WorkspaceSnapshot>()>;
        using Request = cxx::move_only_function<EditorResult<void>(VWorkspaceIntent)>;
        WorkspaceView(object::ObjectDispatcherRef, lux::ui::PaneId, Observe, Request);
        ~WorkspaceView() noexcept override;
        WorkspaceView(const WorkspaceView&) = delete;
        WorkspaceView& operator=(const WorkspaceView&) = delete;
        WorkspaceView(WorkspaceView&&) = delete;
        WorkspaceView& operator=(WorkspaceView&&) = delete;
        [[nodiscard]] EditorResult<void> request(VWorkspaceIntent);
        [[nodiscard]] const WorkspaceSnapshot& snapshot() const noexcept;
        [[nodiscard]] const std::optional<EditorFailure>& observationFailure() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeWorkspaceCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );

    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> makeRecoveryCommands(
        commands::CommandEntry::Query,
        WorkspaceView::Request
    );

} // namespace lux::editor::project
