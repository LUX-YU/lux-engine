#include <lux/engine/editor/gui/scene/OutlinerPane.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <imgui.h>

namespace lux::editor::gui
{
    OutlinerPane::OutlinerPane(scene::SceneEditor &document, std::string id)
        : DocumentPane(document, std::move(id), "Outliner"), selection_(document.selection()),
          selection_connection_(document.observeScoped<scene::SceneEditor::selectionChanged>(
              [this](const scene::SelectionNotice &value) noexcept { selection_ = value; }))
    {
    }

    void OutlinerPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id()});
        selection_ = document_.selection();
        frame.textMuted("Author objects");
        std::size_t index{};
        for (const auto &row : document_.objects())
        {
            ImGui::PushID(static_cast<int>(index++));
            if (row.parent.valid())
            {
                ImGui::Indent();
            }
            if (ImGui::Selectable(row.label.c_str(), row.object == selection_.object))
            {
                static_cast<void>(document_.select(row.object));
            }
            if (row.parent.valid())
            {
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (frame.smallButton("Clear selection"))
        {
            static_cast<void>(document_.select({}));
        }
    }
} // namespace lux::editor::gui
