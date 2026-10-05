#include <exception>
#include <imgui.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kOpenProject{
            commands::CommandIdView{"lux.editor.project.open"},
            "Open Project in New Editor",
            "File"
        };
        constexpr commands::CommandDescriptor kInitialScene{
            commands::CommandIdView{"lux.editor.initial-scene"},
            "Open Initial Scene",
            "File"
        };
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.project.recent"},
            "Recent Projects",
            "File"
        };
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.project.recent"},
             1,
             cxx::typeToken<RecentProjects>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.recent.open"},
             1,
             cxx::typeToken<RecentProjectsView::Open>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
    } // namespace
    desktop::UiResult<std::unique_ptr<lux::ui::Pane>> RecentProjectsView::createConfigured(
        services::ServiceResolver& resolver,
        const desktop::UiCreateInfo& input
    )
    {
        const bool has_content = !input.content.sessions.empty();
        const bool has_configuration = !input.configuration.bytes.empty();
        const bool is_invalid_input = has_content || has_configuration;
        if (is_invalid_input)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::INVALID_CONFIGURATION,
                "project.tool",
                0,
                "This window accepts no author binding or configuration payload"
            });
        }
        auto receiver = resolver.require<Open>(1);
        const bool has_receiver_failure = !receiver && receiver.error().code != services::EServiceError::NOT_FOUND;
        if (has_receiver_failure)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "lux.editor.project.recent.open",
                static_cast<std::uint64_t>(receiver.error().code),
                receiver.error().detail
            });
        }
        const bool is_empty_receiver = receiver && !receiver->get();
        if (is_empty_receiver)
        {
            return cxx::unexpected(
                desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "lux.editor.project.recent.open"}
            );
        }
        auto recent = resolver.require<RecentProjects>(0);
        if (!recent)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "lux.editor.project.recent",
                static_cast<std::uint64_t>(recent.error().code),
                recent.error().detail
            });
        }
        auto pane = std::make_unique<RecentProjectsView>(input.dispatcher, input.instance, recent->get());
        if (receiver)
        {
            auto connection = object::LuxObject::connect(
                pane.get(),
                &RecentProjectsView::openRequested,
                [receiver = *receiver, target = pane.get()](const std::filesystem::path& path) noexcept
                {
                    auto result = receiver.get()(path);
                    if (!result)
                    {
                        target->showFailure(std::move(result.error()));
                    }
                }
            );
            if (!connection)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::FACTORY_FAILURE,
                    "lux.editor.project.recent.open",
                    static_cast<std::uint64_t>(connection.error())
                });
            }
            pane->request_connection_ = std::move(*connection);
        }
        return pane;
    }
    constinit const desktop::UiDescriptor kRecentProjectsView{
        .type = views::ViewTypeIdView{"lux.editor.recent-projects"},
        .label = "Recent Projects",
        .dependencies = kDependencies,
        .create = RecentProjectsView::createConfigured
    };
    struct RecentProjectsView::Impl final
    {
        enum class EAction
        {
            REFRESH,
            RECONCILE
        };
        RecentProjectsView& view_;
        RecentProjects& projects_;
        std::optional<EAction> action_;
        std::optional<std::filesystem::path> opening_;
        std::optional<EditorFailure> failure_;
        struct Content final : lux::ui::Element
        {
            Impl& owner_;
            Content(RecentProjectsView& view, Impl& owner) : Element(view, lux::ui::ElementId{"recent"}), owner_(owner)
            {
                setStretch({1, 1});
            }
            void draw() noexcept override
            {
                if (const auto* error = owner_.projects_.failure())
                {
                    ImGui::TextWrapped("%s: %s", error->domain.c_str(), error->message.c_str());
                }
                if (owner_.failure_)
                {
                    ImGui::TextWrapped("%s: %s", owner_.failure_->domain.c_str(), owner_.failure_->message.c_str());
                }
                if (const auto* result = owner_.projects_.publication())
                {
                    std::visit(
                        [](const auto& value)
                        {
                            if constexpr (!std::same_as<std::decay_t<decltype(value)>, persistence::CommitReceipt>)
                            {
                                ImGui::TextWrapped("Preferences publication: %s", value.failure.detail.c_str());
                            }
                        },
                        *result
                    );
                }
                if (ImGui::Button("Refresh recent projects"))
                {
                    owner_.action_ = EAction::REFRESH;
                }
                if (owner_.projects_.ticket())
                {
                    if (ImGui::Button("Reconcile publication"))
                    {
                        owner_.action_ = EAction::RECONCILE;
                    }
                }
                for (const auto& path : owner_.projects_.entries())
                {
                    const auto text = path.generic_u8string();
                    if (ImGui::Button(reinterpret_cast<const char*>(text.c_str())))
                    {
                        owner_.opening_ = path; // Owned input survives a subsequent catalog refresh.
                    }
                }
            }
        } content_;

        Impl(RecentProjectsView& view, RecentProjects& projects)
            : view_(view), projects_(projects), content_(view, *this)
        {
            if (!view_.setContent(content_))
            {
                std::terminate(); // Fixed content in a detached Pane.
            }
        }
        void update() noexcept
        {
            if (const auto action = std::exchange(action_, {}))
            {
                auto result = *action == EAction::REFRESH ? projects_.refresh() : projects_.reconcile();
                if (!result)
                {
                    failure_ = std::move(result.error());
                }
                else
                {
                    failure_.reset();
                }
            }
            if (auto path = std::exchange(opening_, {}))
            {
                auto delivered = view_.requestOpen(std::move(*path));
                if (!delivered)
                {
                    failure_ = std::move(delivered.error());
                }
            }
        }
    };
    RecentProjectsView::RecentProjectsView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        RecentProjects& projects
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kRecentProjectsView.type.name()}, "Recent Projects"),
          impl_(std::make_unique<Impl>(*this, projects))
    {
    }
    RecentProjectsView::~RecentProjectsView() noexcept = default;
    void RecentProjectsView::update() noexcept
    {
        impl_->update();
    }
    EditorResult<void> RecentProjectsView::requestOpen(std::filesystem::path path)
    {
        if (!emit(openRequested, std::move(path)).complete())
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recent.open.delivery"});
        }
        return {};
    }
    void RecentProjectsView::showFailure(EditorFailure failure)
    {
        impl_->failure_ = std::move(failure);
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    std::shared_ptr<commands::CommandEntry> makeRecentProjectsCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kRecentProjectsView>(std::move(query), std::move(open));
    }

    std::shared_ptr<commands::CommandEntry> makeOpenProjectCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>()> request
    )
    {
        return workbench::detail::bindCommand<kOpenProject>(
            std::move(query),
            [request = std::move(request)](const commands::CommandInvocation&) mutable { return request(); }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeInitialSceneCommand(
        commands::CommandEntry::Query query,
        ProjectStorage& project,
        cxx::move_only_function<commands::CommandResult<void>(AssetReference)> open
    )
    {
        return workbench::detail::bindCommand<kInitialScene>(
            [query = std::move(query),
             &project](const commands::CommandQuery& input) mutable -> commands::CommandResult<commands::CommandState>
            {
                auto state = query(input);
                if (state)
                {
                    state->enabled = state->enabled && !project.manifest().default_scene.empty();
                }
                return state;
            },
            [&project,
             open = std::move(open)](const commands::CommandInvocation&) mutable -> commands::CommandResult<void>
            {
                auto reference = initialSceneReference(project);
                if (!reference)
                {
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::INVALID_ARGUMENT,
                        reference.error().domain,
                        reference.error().reason,
                        reference.error().message
                    });
                }
                return open(*reference);
            }
        );
    }

} // namespace lux::editor::project
