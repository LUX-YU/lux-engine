#include <lux/engine/editor/gui/scene/InspectorPane.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <imgui.h>
#include <algorithm>

namespace lux::editor::gui
{
    InspectorPane::InspectorPane(scene::SceneEditor &document, std::string id)
        : DocumentPane(document, std::move(id), "Inspector"), readers_(firstPartySceneReaders()),
          selection_(document.selection()),
          selection_connection_(document.observeScoped<scene::SceneEditor::selectionChanged>(
              [this](const scene::SelectionNotice &value) noexcept
              {
                  selection_ = value;
                  directory_dirty_ = true;
              }))
    {
    }

    void InspectorPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id()});
        frame.textMuted("Read-only inspection");
        const auto current = document_.selection();
        if (current.revision != selection_.revision)
        {
            selection_ = current;
            directory_dirty_ = true;
        }
        if (directory_dirty_)
        {
            components_ = document_.components(selection_.object);
            directory_dirty_ = false;
        }
        if (!selection_.object.valid())
        {
            frame.textMuted("Select an object in the Outliner");
            return;
        }
        for (const auto &component : components_)
        {
            const auto reader = std::ranges::find(readers_, component.type, &ComponentReadBinding::type);
            const auto &title = reader == readers_.end() ? component.name : reader->name;
            if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto table = frame.table({lux::ui::WidgetIdView{component.name}, 2, false, false, false, 110});
                if (table.visible() && reader != readers_.end())
                {
                    reader->draw(document_, selection_.object, frame);
                }
            }
        }
    }
} // namespace lux::editor::gui
