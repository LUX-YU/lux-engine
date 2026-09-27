#include <lux/engine/editor/project/ProjectPane.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/AssetOpenRequest.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Root.hpp>
#include <imgui.h>
#include <algorithm>
namespace lux::editor
{
    ProjectPane::ProjectPane(lux::ui::Root& root, EditorContext& context, EditorResult<void>& status)
        : Pane(root, lux::ui::PaneId{"project"}, lux::ui::PaneTypeId{"lux.editor.project"}, "Project"), content_(*this),
          context_(context)
    {
        setContent(content_);
        close_connection_ = detail::takeConnection(
            object::LuxObject::connect(
                this,
                &lux::ui::Pane::closeRequested,
                [this]() noexcept { hide_requested_ = true; }
            ),
            status
        );
    }
    void ProjectPane::Content::draw() noexcept
    {
        if (ImGui::BeginChild("project-content", {rect().size.width, rect().size.height}))
            owner_.drawActions();
        ImGui::EndChild();
    }
    void ProjectPane::update() noexcept
    {
        if (std::exchange(hide_requested_, false))
            setVisible(false);
    }
    void ProjectPane::drawActions() noexcept
    {
        ImGui::BeginDisabled(context_.panes().frozen());
        for (const auto& entry : context_.project().manifest().assets)
            if (ImGui::Button(entry.source_path.c_str()))
            {
                AssetOpenRequest request{entry.id};
                static_cast<void>(object::routeEvent(*this, root(), request));
            }
        ImGui::EndDisabled();
    }
}
