#include <lux/engine/editor/project/SettingsView.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <algorithm>
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
            Content(SettingsView& view, Impl& owner) : Element(view, lux::ui::ElementId{"settings"}), data(owner)
            {
                setStretch({1, 1});
            }
            void draw() noexcept override
            {
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
                            data.selection.erase(found);
                        else
                        {
                            auto original =
                                std::ranges::find(data.baseline, plugin.identity.id, &ProjectPluginEntry::id);
                            if (original != data.baseline.end())
                                data.selection.push_back(*original);
                            else
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
                    data.action = EAction::SAVE;
                ImGui::SameLine();
                if (ImGui::Button("Revert draft"))
                    data.action = EAction::REVERT;
                ImGui::EndDisabled();
                if (data.status)
                {
                    if (const auto* error = std::get_if<EditorFailure>(&*data.status))
                    {
                        ImGui::TextWrapped("%s: %s", error->domain.c_str(), error->message.c_str());
                        if (ImGui::Button("Retry / reconcile"))
                            data.action = EAction::RETRY;
                    }
                    else if (const auto* saved = std::get_if<PublicationSucceeded>(&*data.status))
                    {
                        ImGui::TextUnformatted("Project selection published. Active plugin code remains unchanged.");
                        if (!saved->cleanup)
                            ImGui::TextWrapped("%s", saved->cleanup.error().domain.c_str());
                    }
                    else if (std::holds_alternative<PublicationAbandoned>(*data.status))
                        ImGui::TextUnformatted("Publication abandoned.");
                    else
                        ImGui::TextUnformatted("Publishing project selection...");
                    if (ImGui::Button("Abandon publication"))
                        data.action = EAction::ABANDON;
                    ImGui::SameLine();
                    if (ImGui::Button("Acknowledge result"))
                        data.action = EAction::ACKNOWLEDGE;
                }
                if (data.failure)
                    ImGui::TextWrapped("%s", data.failure->domain.c_str());
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
        {}
    };
    SettingsView::SettingsView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ProjectStorage& project,
        const lux::project::PluginManager& plugins
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.settings"}, "Settings"),
          impl_(std::make_unique<Impl>(*this, project, plugins))
    {
        setContent(impl_->content);
    }
    SettingsView::~SettingsView() noexcept = default;
    EditorResult<void> SettingsView::requestSave(std::vector<ProjectPluginEntry> selected)
    {
        impl_->selection = std::move(selected);
        if (!emit(selectionRequested, PluginSelectionDraft{impl_->baseline, impl_->selection}).complete())
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.delivery"});
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
            return;
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
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.retry"});
            break;
        case Impl::EAction::ABANDON:
            if (!emit(abandonRequested).complete())
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.abandon"});
            break;
        case Impl::EAction::ACKNOWLEDGE:
            if (!emit(acknowledgeRequested).complete())
                result = cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.acknowledge"});
            break;
        }
        if (!result)
            impl_->failure = result.error();
        else
            impl_->failure.reset();
    }
}
