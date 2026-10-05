#include <algorithm>
#include <exception>
#include <imgui.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/ui/Element.hpp>
namespace lux::editor::project
{
    struct SettingsView::Impl final
    {
        enum class EAction
        {
            SAVE,
            REVERT,
            RETRY,
            ABANDON,
            ACKNOWLEDGE
        };
        struct Content final : lux::ui::Element
        {
            Impl& data;
            std::unique_ptr<SettingsContent> settings;
            Content(SettingsView& view, Impl& owner) : Element(view, lux::ui::ElementId{"settings"}), data(owner)
            {
                setStretch({1, 1});
            }
            void arrangeContent() noexcept override
            {
                if (settings)
                {
                    settings->arrange({{}, {rect().size.width, std::max(360.f, rect().size.height * 0.7f)}});
                }
            }
            void draw() noexcept override
            {
                if (settings &&
                    ImGui::CollapsingHeader("Personal and extension settings", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const float y = ImGui::GetCursorPosY() - rect().position.y;
                    drawChild(*settings, {0, y});
                    ImGui::SetCursorPosY(y + rect().position.y + settings->rect().size.height);
                }
                if (!ImGui::CollapsingHeader("Project plugins"))
                {
                    return;
                }
                ImGui::TextWrapped(
                    "Plugin selection is saved to the project. Changes take effect next time the project opens."
                );
                ImGui::BeginDisabled(data.status.has_value() || !data.project.writable());
                for (const auto& plugin : data.plugins.catalog().plugins())
                {
                    auto found = std::ranges::find(data.selection, plugin.identity.id, &ProjectPluginEntry::id);
                    bool selected = found != data.selection.end();
                    if (ImGui::Checkbox(plugin.identity.id.c_str(), &selected))
                    {
                        if (!selected)
                        {
                            data.selection.erase(found);
                        }
                        else
                        {
                            auto original =
                                std::ranges::find(data.baseline, plugin.identity.id, &ProjectPluginEntry::id);
                            if (original != data.baseline.end())
                            {
                                data.selection.push_back(*original);
                            }
                            else
                            {
                                data.selection.push_back(
                                    {plugin.identity.id,
                                     plugin.identity.version,
                                     plugin.root == data.project.root()
                                         ? plugin.description_file.lexically_relative(data.project.root())
                                               .generic_string()
                                         : std::string{}}
                                );
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "v%u%s%s",
                        plugin.identity.version,
                        plugin.builtin ? " (builtin)" : " (extension)",
                        data.plugins.find(plugin.identity.id) ? " (active)" : ""
                    );
                    ImGui::TextWrapped("%s", plugin.description.c_str());
                }
                if (ImGui::Button("Save selection"))
                {
                    data.action = EAction::SAVE;
                }
                ImGui::SameLine();
                if (ImGui::Button("Revert draft"))
                {
                    data.action = EAction::REVERT;
                }
                ImGui::EndDisabled();
                if (data.status)
                {
                    if (const auto* error = std::get_if<EditorFailure>(&*data.status))
                    {
                        ImGui::TextWrapped("%s: %s", error->domain.c_str(), error->message.c_str());
                        if (ImGui::Button("Retry / reconcile"))
                        {
                            data.action = EAction::RETRY;
                        }
                    }
                    else if (const auto* saved = std::get_if<PublicationSucceeded>(&*data.status))
                    {
                        ImGui::TextUnformatted("Project selection published. Active plugin code remains unchanged.");
                        if (!saved->cleanup)
                        {
                            ImGui::TextWrapped("%s", saved->cleanup.error().domain.c_str());
                        }
                    }
                    else if (std::holds_alternative<PublicationAbandoned>(*data.status))
                    {
                        ImGui::TextUnformatted("Publication abandoned.");
                    }
                    else
                    {
                        ImGui::TextUnformatted("Publishing project selection...");
                    }
                    if (ImGui::Button("Abandon publication"))
                    {
                        data.action = EAction::ABANDON;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Acknowledge result"))
                    {
                        data.action = EAction::ACKNOWLEDGE;
                    }
                }
                if (data.failure)
                {
                    ImGui::TextWrapped("%s", data.failure->domain.c_str());
                }
            }
        } content;
        ProjectStorage& project;
        const lux::project::PluginManager& plugins;
        std::vector<ProjectPluginEntry> baseline, selection;
        std::optional<VPublicationStatus> status;
        std::optional<EditorFailure> failure;
        std::optional<EAction> action;
        Impl(SettingsView& view, ProjectStorage& project, const lux::project::PluginManager& plugins)
            : content(view, *this), project(project), plugins(plugins), baseline(project.manifest().plugins),
              selection(baseline)
        {
        }
    };
    SettingsView::SettingsView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ProjectStorage& project,
        const lux::project::PluginManager& plugins,
        std::shared_ptr<SettingsContentInput> input
    )
        : Pane(
              dispatcher,
              std::move(id),
              lux::ui::PaneTypeId{descriptor().type.name()},
              std::string{descriptor().label}
          ),
          impl_(std::make_unique<Impl>(*this, project, plugins))
    {
        if (input)
        {
            impl_->content.settings = std::make_unique<SettingsContent>(
                impl_->content,
                lux::ui::ElementId{"settings-values"},
                std::move(input)
            );
        }
        if (!setContent(impl_->content))
        {
            std::terminate(); // Fixed content in a detached Pane.
        }
    }
    SettingsView::~SettingsView() noexcept = default;
    EditorResult<void> SettingsView::requestSave(std::vector<ProjectPluginEntry> selected)
    {
        impl_->selection = std::move(selected);
        if (!emit(selectionRequested, PluginSelectionDraft{impl_->baseline, impl_->selection}).complete())
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.delivery"});
        }
        return {};
    }
    void SettingsView::setPublicationStatus(std::optional<VPublicationStatus> status)
    {
        if (status && std::holds_alternative<PublicationSucceeded>(*status))
        {
            impl_->baseline = impl_->project.manifest().plugins;
            impl_->selection = impl_->baseline;
        }
        impl_->status = std::move(status);
    }
    void SettingsView::update() noexcept
    {
        const auto action = std::exchange(impl_->action, {});
        if (!action)
        {
            return;
        }
        EditorResult<void> result;
        switch (*action)
        {
        case Impl::EAction::SAVE:
            result = requestSave(impl_->selection);
            break;
        case Impl::EAction::REVERT:
            impl_->baseline = impl_->project.manifest().plugins;
            impl_->selection = impl_->baseline;
            break;
        case Impl::EAction::RETRY:
            if (!emit(retryRequested).complete())
            {
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.retry"});
            }
            break;
        case Impl::EAction::ABANDON:
            if (!emit(abandonRequested).complete())
            {
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.abandon"});
            }
            break;
        case Impl::EAction::ACKNOWLEDGE:
            if (!emit(acknowledgeRequested).complete())
            {
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.acknowledge"});
            }
            break;
        }
        if (!result)
        {
            impl_->failure = result.error();
        }
        else
        {
            impl_->failure.reset();
        }
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    namespace
    {
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.settings"},
            "Settings",
            cxx::typeToken<std::monostate>()
        };
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.settings"},
            "Project Settings",
            "Window"
        };
    } // namespace
    const views::ViewFactoryDescriptor& SettingsView::descriptor() noexcept
    {
        return kFactoryDescriptor;
    }
    namespace
    {
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.project.storage"},
             1,
             cxx::typeToken<ProjectStorage>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.project.plugins"},
             1,
             cxx::typeToken<lux::project::PluginManager>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.settings.content"},
             1,
             cxx::typeToken<std::shared_ptr<SettingsContentInput>>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
        desktop::UiResult<std::unique_ptr<lux::ui::Pane>> createView(
            services::ServiceResolver& resolver,
            const desktop::UiCreateInfo& input
        )
        {
            const bool has_content = !input.content.sessions.empty();
            const bool has_configuration = !input.configuration.bytes.empty();
            const bool is_invalid_input = has_content || has_configuration;
            if (is_invalid_input)
            {
                return cxx::unexpected(desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "settings.input"});
            }
            auto project = resolver.require<ProjectStorage>(0);
            auto plugins = resolver.require<lux::project::PluginManager>(1);
            auto settings = resolver.require<std::shared_ptr<SettingsContentInput>>(2);
            const bool is_missing_project = !project;
            const bool is_missing_plugins = !plugins;
            const bool has_missing_dependency = is_missing_project || is_missing_plugins;
            if (has_missing_dependency)
            {
                const auto& error = !project ? project.error() : plugins.error();
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::DEPENDENCY,
                    "settings.provider",
                    static_cast<std::uint64_t>(error.code),
                    error.detail
                });
            }
            if (!settings && settings.error().code != services::EServiceError::NOT_FOUND)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::DEPENDENCY,
                    "settings.content",
                    static_cast<std::uint64_t>(settings.error().code),
                    settings.error().detail
                });
            }
            return std::make_unique<SettingsView>(
                input.dispatcher,
                input.instance,
                project->get(),
                plugins->get(),
                settings ? settings->get() : std::shared_ptr<SettingsContentInput>{}
            );
        }
    } // namespace
    constinit const desktop::UiDescriptor kSettingsView{
        .type = kFactoryDescriptor.type,
        .label = kFactoryDescriptor.label,
        .dependencies = kDependencies,
        .create = createView
    };
    std::shared_ptr<views::ViewFactoryEntry> makeSettingsViewFactory(
        ProjectStorage& project,
        const lux::project::PluginManager& plugins,
        cxx::move_only_function<void(const PluginSelectionDraft&)> save,
        cxx::move_only_function<void()> retry,
        cxx::move_only_function<void()> abandon,
        cxx::move_only_function<void()> acknowledge,
        std::shared_ptr<SettingsContentInput> settings
    )
    {
        struct Receivers final
        {
            cxx::move_only_function<void(const PluginSelectionDraft&)> save;
            cxx::move_only_function<void()> retry, abandon, acknowledge;
        };
        auto receivers =
            std::make_shared<Receivers>(std::move(save), std::move(retry), std::move(abandon), std::move(acknowledge));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(
            lux::object::CodeLease::builtin(),
            [&project, &plugins, receivers, settings](const views::ViewFactoryInput& input
            ) -> views::ViewFactoryResult<views::DetachedView>
            {
                auto pane =
                    std::make_unique<SettingsView>(input.dispatcher(), input.paneId(), project, plugins, settings);
                std::array<object::LuxObject::ConnectResult, 4> bindings{
                    object::LuxObject::connect(
                        pane.get(),
                        &SettingsView::selectionRequested,
                        [receivers](const PluginSelectionDraft& value) noexcept { receivers->save(value); }
                    ),
                    object::LuxObject::connect(
                        pane.get(),
                        &SettingsView::retryRequested,
                        [receivers]() noexcept { receivers->retry(); }
                    ),
                    object::LuxObject::connect(
                        pane.get(),
                        &SettingsView::abandonRequested,
                        [receivers]() noexcept { receivers->abandon(); }
                    ),
                    object::LuxObject::connect(
                        pane.get(),
                        &SettingsView::acknowledgeRequested,
                        [receivers]() noexcept { receivers->acknowledge(); }
                    )
                };
                for (const auto& binding : bindings)
                {
                    if (!binding)
                    {
                        return cxx::unexpected(workbench::detail::viewFailure(binding.error()));
                    }
                }
                views::DetachedView result{lux::object::CodeLease::builtin(), std::move(pane)};
                for (auto& binding : bindings)
                {
                    result.addConnection(std::move(*binding));
                }
                return result;
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeSettingsCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }
} // namespace lux::editor::project
