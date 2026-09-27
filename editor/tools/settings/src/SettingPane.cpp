#include <lux/engine/editor/settings/SettingPane.hpp>
#include <lux/engine/editor/settings/detail/PluginSettingsElement.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/WorkspaceRequest.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Root.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <optional>
namespace lux::editor
{
    class SettingPane::Content final : public lux::ui::Element
    {
    public:
        Content(SettingPane& pane, EditorContext& context)
            : Element(pane, lux::ui::ElementId{"settings"}), plugins_(*this, context)
        {}

    private:
        void arrangeContent() noexcept override
        {
            plugins_.arrange({{0, 32}, {rect().size.width, std::max(1.0F, rect().size.height - 32)}});
        }
        void draw() noexcept override
        {
            if (!ImGui::BeginTabBar("settings-tabs"))
                return;
            if (ImGui::BeginTabItem("Plugins"))
            {
                drawChild(plugins_);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Layouts"))
            {
                ImGui::TextWrapped("Layouts restore window placement without replacing unsaved content.");
                ImGui::TextWrapped("%s", message_.c_str());
                ImGui::InputText("Name", &name_);
                ImGui::InputText("Rename to", &new_name_);
                ImGui::BeginDisabled(pending_);
                if (ImGui::Button("Save current layout"))
                    action_ = EWorkspaceAction::SAVE;
                ImGui::SameLine();
                if (ImGui::Button("Restore default"))
                    action_ = EWorkspaceAction::DEFAULT;
                for (const auto& name : layouts_)
                    if (ImGui::Selectable(name.c_str(), name_ == name))
                        name_ = name;
                if (ImGui::Button("Apply"))
                    action_ = EWorkspaceAction::APPLY;
                ImGui::SameLine();
                if (ImGui::Button("Rename"))
                    action_ = EWorkspaceAction::RENAME;
                ImGui::SameLine();
                if (ImGui::Button("Delete layout"))
                    action_ = EWorkspaceAction::REMOVE;
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        void update() noexcept override
        {
            if (action_)
            {
                WorkspaceRequest request{*action_, name_, new_name_};
                const bool accepted = object::routeEvent(*this, root(), request);
                if (!accepted)
                    message_ = "Workspace commands are not available in this host";
                else if (!request.result)
                    message_ = request.result.error().domain + ": " + request.result.error().message;
                pending_ = accepted && request.pending;
                action_.reset();
            }
            WorkspaceRequest status;
            status.revision = revision_;
            if (object::routeEvent(*this, root(), status))
            {
                if (status.revision != revision_)
                {
                    layouts_ = std::move(status.layouts);
                    revision_ = status.revision;
                }
                pending_ = status.pending;
                if (!status.message.empty())
                    message_ = std::move(status.message);
            }
        }
        PluginSettingsElement plugins_;
        std::vector<std::string> layouts_;
        std::string name_, new_name_, message_;
        std::optional<EWorkspaceAction> action_;
        std::uint64_t revision_{UINT64_MAX};
        bool pending_{};
    };
    SettingPane::SettingPane(lux::ui::Root& root, EditorContext& context, EditorResult<void>& status)
        : Pane(root, lux::ui::PaneId{"settings"}, lux::ui::PaneTypeId{"lux.editor.settings"}, "Settings"),
          content_(std::make_unique<Content>(*this, context))
    {
        setContent(*content_);
        close_connection_ = detail::takeConnection(
            object::LuxObject::connect(
                this,
                &lux::ui::Pane::closeRequested,
                [this]() noexcept { hide_requested_ = true; }
            ),
            status
        );
    }
    SettingPane::~SettingPane() = default;
    void SettingPane::update() noexcept
    {
        if (std::exchange(hide_requested_, false))
            setVisible(false);
    }
}
