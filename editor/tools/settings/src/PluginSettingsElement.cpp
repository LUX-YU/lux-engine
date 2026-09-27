#include <lux/engine/editor/settings/detail/PluginSettingsElement.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <imgui.h>
#include <algorithm>
namespace lux::editor
{
    PluginSettingsElement::PluginSettingsElement(lux::ui::Element& pane, EditorContext& context)
        : Element(pane, lux::ui::ElementId{"plugins"}), project_(context.project()), plugins_(context.plugins()),
          execution_(context.execution()), selection_(project_.manifest().plugins)
    {}
    void PluginSettingsElement::draw() noexcept
    {
        ImGui::TextWrapped("Saved changes take effect the next time this project is opened.");
        if (!status_.empty())
            ImGui::TextWrapped("%s", status_.c_str());
        const auto* saving = project_.pluginSaveStatus();
        ImGui::BeginDisabled(saving || !project_.writable());
        for (const auto& plugin : plugins_.catalog().plugins())
        {
            const auto found = std::ranges::find(selection_, plugin.identity.id, &ProjectPluginEntry::id);
            bool selected = found != selection_.end();
            if (ImGui::Checkbox(plugin.identity.id.c_str(), &selected))
            {
                if (selected)
                {
                    const auto original =
                        std::ranges::find(project_.manifest().plugins, plugin.identity.id, &ProjectPluginEntry::id);
                    if (original != project_.manifest().plugins.end())
                        selection_.push_back(*original);
                    else
                    {
                        std::string description;
                        if (plugin.root == project_.root())
                            description = plugin.description_file.lexically_relative(project_.root()).generic_string();
                        selection_.push_back({plugin.identity.id, plugin.identity.version, std::move(description)});
                    }
                }
                else
                    selection_.erase(found);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("v%u%s", plugin.identity.version, plugins_.find(plugin.identity.id) ? " (active)" : "");
            if (!plugin.description.empty())
                ImGui::TextWrapped("%s", plugin.description.c_str());
        }
        if (ImGui::Button("Save plugin selection"))
            save_requested_ = true;
        ImGui::EndDisabled();
        if (saving && std::holds_alternative<EditorFailure>(*saving))
        {
            ImGui::TextWrapped("%s", std::get<EditorFailure>(*saving).message.c_str());
            if (ImGui::Button("Retry save"))
                retry_requested_ = true;
            ImGui::SameLine();
            if (ImGui::Button("Abandon save"))
                abandon_requested_ = true;
        }
    }

    void PluginSettingsElement::update() noexcept
    {
        const auto report = [&](const EditorResult<void>& result) {
            if (!result)
                status_ = result.error().domain + ": " + result.error().message;
        };
        if (std::exchange(save_requested_, false))
            report(project_.savePlugins(selection_, execution_));
        if (std::exchange(retry_requested_, false))
            report(project_.retryPluginSave());
        if (std::exchange(abandon_requested_, false))
            project_.abandonPluginSave();
        if (const auto* status = project_.pluginSaveStatus())
        {
            if (const auto* saved = std::get_if<PublicationSucceeded>(status))
            {
                status_ = "Plugin selection saved; reopen the project to apply it.";
                report(saved->cleanup);
                report(project_.acknowledgePluginSave());
            }
            else if (std::holds_alternative<PublicationAbandoned>(*status))
            {
                status_ = "Plugin selection was not changed.";
                report(project_.acknowledgePluginSave());
            }
        }
    }
}
