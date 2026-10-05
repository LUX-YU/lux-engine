#pragma once
namespace lux::editor::views
{
    class ViewFactoryEntry;
    struct ViewFactoryDescriptor;
} // namespace lux::editor::views
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/persistence/SaveTypes.hpp>
#include <lux/engine/editor/scene/RunTypes.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::desktop
{
    struct UiDescriptor;
}
namespace lux::editor::views
{
    class ViewFactoryEntry;
}

namespace lux::editor::project
{
    extern const desktop::UiDescriptor kResultsView;
    struct AcknowledgeMaintenance final
    {
    };
    struct AcknowledgeSave final
    {
        persistence::SaveId target;
    };
    struct AcknowledgeArtifact final
    {
        std::uint64_t target;
    };
    struct RetryArtifact final
    {
        std::uint64_t target;
    };
    struct AbandonArtifact final
    {
        std::uint64_t target;
    };
    struct CancelSave final
    {
        persistence::SaveId target;
    };
    struct ReconcilePublication final
    {
        persistence::WriteTicket target;
    };
    struct AcknowledgeReload final
    {
        sessions::ContentStamp target;
    };
    struct AcknowledgeModel final
    {
        std::uint64_t target;
    };
    struct AcknowledgeRunFailure final
    {
        scene::StartRunId target;
    };
    struct AcknowledgeStep final
    {
        scene::StepTicket target;
    };
    struct CancelModel final
    {
        std::uint64_t target;
    };
    struct ShowContent final
    {
        sessions::ContentStamp target;
    };
    struct SaveContentAs final
    {
        sessions::ContentStamp target;
    };
    struct AcknowledgeSaveAll final
    {
    };
    using VResultIntent = std::variant<
        AcknowledgeMaintenance,
        AcknowledgeSave,
        AcknowledgeArtifact,
        RetryArtifact,
        AbandonArtifact,
        CancelSave,
        ReconcilePublication,
        AcknowledgeReload,
        AcknowledgeModel,
        AcknowledgeRunFailure,
        AcknowledgeStep,
        CancelModel,
        ShowContent,
        SaveContentAs,
        AcknowledgeSaveAll>;

    struct ResultAction final
    {
        std::string label;
        VResultIntent intent;
    };
    struct ResultRow final
    {
        std::string key;
        std::vector<std::string> messages;
        std::vector<ResultAction> actions;
    };
    struct ResultSection final
    {
        std::string title;
        std::vector<ResultRow> rows;
    };
    struct ResultsSnapshot final
    {
        std::vector<ResultSection> sections;
    };

    // A presentation snapshot is not a second result owner. Requests carry their original
    // domain identities; the receiver revalidates and executes them at its safe point.
    class ResultsView final : public lux::ui::Pane
    {
    public:
        using Observe = cxx::move_only_function<EditorResult<ResultsSnapshot>()>;
        using Request = cxx::move_only_function<EditorResult<void>(VResultIntent)>;
        ResultsView(object::ObjectDispatcherRef, lux::ui::PaneId, Observe, Request);
        ~ResultsView() noexcept override;
        ResultsView(const ResultsView&) = delete;
        ResultsView& operator=(const ResultsView&) = delete;
        ResultsView(ResultsView&&) = delete;
        ResultsView& operator=(ResultsView&&) = delete;
        [[nodiscard]] EditorResult<void> request(VResultIntent);
        [[nodiscard]] const ResultsSnapshot& snapshot() const noexcept;
        [[nodiscard]] const std::optional<EditorFailure>& observationFailure() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeResultsViewFactory(
        ResultsView::Observe observe,
        ResultsView::Request request
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeResultsCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );

} // namespace lux::editor::project
