#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>

namespace lux::editor::project
{
    struct RecentProjectsView::Impl final
    {
        enum class EAction { REFRESH, RECONCILE };
        RecentProjectsView& view_;
        RecentProjects& projects_;
        std::optional<EAction> action_;
        std::optional<std::filesystem::path> opening_;
        std::optional<EditorFailure> failure_;
        struct Content final : lux::ui::Element
        {
            Impl& owner_;
            Content(RecentProjectsView& view, Impl& owner)
                : Element(view, lux::ui::ElementId{"recent"}), owner_(owner)
            {
                setStretch({1, 1});
            }
            void draw() noexcept override
            {
                if (const auto* error = owner_.projects_.failure())
                    ImGui::TextWrapped("%s: %s", error->domain.c_str(), error->message.c_str());
                if (owner_.failure_)
                    ImGui::TextWrapped("%s: %s", owner_.failure_->domain.c_str(), owner_.failure_->message.c_str());
                if (const auto* result = owner_.projects_.publication())
                    std::visit([](const auto& value) {
                        if constexpr (!std::same_as<std::decay_t<decltype(value)>, persistence::CommitReceipt>)
                            ImGui::TextWrapped("Preferences publication: %s", value.failure.detail.c_str());
                    }, *result);
                if (ImGui::Button("Refresh recent projects"))
                    owner_.action_ = EAction::REFRESH;
                if (owner_.projects_.ticket())
                    if (ImGui::Button("Reconcile publication"))
                        owner_.action_ = EAction::RECONCILE;
                for (const auto& path : owner_.projects_.entries())
                {
                    const auto text = path.generic_u8string();
                    if (ImGui::Button(reinterpret_cast<const char*>(text.c_str())))
                        owner_.opening_ = path; // Owned input survives a subsequent catalog refresh.
                }
            }
        } content_;

        Impl(RecentProjectsView& view, RecentProjects& projects)
            : view_(view), projects_(projects), content_(view, *this)
        {
            view_.setContent(content_);
        }
        void update() noexcept
        {
            if (const auto action = std::exchange(action_, {}))
            {
                auto result = *action == EAction::REFRESH ? projects_.refresh() : projects_.reconcile();
                if (!result)
                    failure_ = std::move(result.error());
                else
                    failure_.reset();
            }
            if (auto path = std::exchange(opening_, {}))
            {
                auto delivered = view_.emit(view_.openRequested, std::move(*path));
                if (!delivered.complete())
                    failure_ = EditorFailure{EEditorError::BUSY, "recent.open.delivery"};
            }
        }
    };
    RecentProjectsView::RecentProjectsView(
        object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, RecentProjects& projects
    ) : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.recent-projects"}, "Recent Projects"),
        impl_(std::make_unique<Impl>(*this, projects))
    {}
    RecentProjectsView::~RecentProjectsView() noexcept = default;
    void RecentProjectsView::update() noexcept { impl_->update(); }
    void RecentProjectsView::showFailure(EditorFailure failure) { impl_->failure_ = std::move(failure); }
}

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kOpenProject{
            commands::CommandIdView{"lux.editor.project.open"}, "Open Project in New Editor", "File"
        };
        constexpr commands::CommandDescriptor kInitialScene{
            commands::CommandIdView{"lux.editor.initial-scene"}, "Open Initial Scene", "File"
        };
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.project.recent"}, "Recent Projects", "File"
        };
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.recent-projects"}, "Recent Projects", cxx::typeToken<std::monostate>()
        };
    }
    std::shared_ptr<views::ViewFactoryEntry> makeRecentProjectsViewFactory(
        RecentProjects& recent, cxx::move_only_function<EditorResult<void>(const std::filesystem::path&)> open
    )
    {
        auto receiver = std::make_shared<cxx::move_only_function<EditorResult<void>(const std::filesystem::path&)>>(
            std::move(open)
        );
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(contracts::CodeLease::builtin(),
            [&recent, receiver](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                auto pane = std::make_unique<RecentProjectsView>(input.dispatcher(), input.paneId(), recent);
                auto connection = object::LuxObject::connect(pane.get(), &RecentProjectsView::openRequested,
                    [target = pane.get(), receiver](const std::filesystem::path& path) noexcept {
                        auto result = (*receiver)(path);
                        if (!result)
                            target->showFailure(std::move(result.error()));
                    }
                );
                if (!connection)
                    return cxx::unexpected(workbench::detail::viewFailure(connection.error()));
                views::DetachedView view{contracts::CodeLease::builtin(), std::move(pane)};
                view.addConnection(std::move(*connection));
                return view;
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeRecentProjectsCommand(
        commands::CommandEntry::Query query, desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }

    std::shared_ptr<commands::CommandEntry> makeOpenProjectCommand(
        commands::CommandEntry::Query query, cxx::move_only_function<commands::CommandResult<void>()> request
    )
    {
        return workbench::detail::bindCommand<kOpenProject>(std::move(query),
            [request = std::move(request)](const commands::CommandInvocation&) mutable { return request(); }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeInitialSceneCommand(
        commands::CommandEntry::Query query, ProjectStorage& project,
        cxx::move_only_function<commands::CommandResult<void>(AssetReference)> open
    )
    {
        return workbench::detail::bindCommand<kInitialScene>(
            [query = std::move(query), &project](const commands::CommandQuery& input) mutable
                -> commands::CommandResult<commands::CommandState> {
                auto state = query(input);
                if (state)
                    state->enabled = state->enabled && !project.manifest().default_scene.empty();
                return state;
            }, [&project, open = std::move(open)](const commands::CommandInvocation&) mutable
                -> commands::CommandResult<void> {
                auto reference = initialSceneReference(project);
                if (!reference)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::INVALID_ARGUMENT, reference.error().domain,
                        reference.error().reason, reference.error().message
                    });
                return open(*reference);
            }
        );
    }

}
