#include <algorithm>
#include <imgui.h>
#include <lux/engine/editor/gui/scene/InspectorPane.hpp>
#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::gui
{
InspectorPane::InspectorPane(scene::SceneEditor &document, std::string id,
                             std::shared_ptr<const std::vector<ComponentBinding>> bindings)
    : DocumentPane(document, id, "Inspector"), bindings_(std::move(bindings)), interaction_(document, std::move(id)),
      selection_(document.selection()),
      selection_connection_(document.observeScoped<scene::SceneEditor::selectionChanged>(
          [this](const scene::SelectionNotice &value) noexcept {
              selection_ = value;
              directory_dirty_ = true;
          })),
      objects_connection_(document.observeScoped<scene::SceneEditor::objectsChanged>(
          [this](editing::Revision) noexcept { directory_dirty_ = true; }))
{
}

void InspectorPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
{
    context.activateContext(lux::ui::UiContextIdView{id()});
    const auto history = document_.historyView();
    frame.textMuted(history && history->history.clean ? "Scene editing" : "Scene editing | Unsaved changes");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && interaction_.active())
    {
        static_cast<void>(interaction_.finish(document_, false));
        return;
    }
    const auto current = document_.selection();
    if (current.revision != selection_.revision)
    {
        selection_ = current;
        directory_dirty_ = true;
    }
    if (directory_dirty_)
    {
        if (!interaction_.finish(document_, true))
        {
            frame.text(interaction_.error.data());
            return;
        }
        interaction_.reset();
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
        const auto binding = std::ranges::lower_bound(*bindings_, component.type.hash(), {},
                                                      [](const ComponentBinding &value) { return value.type.hash(); });
        const bool has_binding = binding != bindings_->end() && binding->type == component.type;
        const auto title = has_binding ? binding->name.c_str() : component.name.c_str();
        if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto table = frame.table({lux::ui::WidgetIdView{component.name}, 2, false, false, false, 110});
            if (table.visible() && has_binding)
            {
                binding->draw(document_, selection_.object, frame, interaction_);
            }
        }
    }
    if (interaction_.error[0])
    {
        frame.text(interaction_.error.data());
    }
}

void InspectorPane::requestClose() noexcept
{
    if (interaction_.finish(document_, false))
    {
        DocumentPane::requestClose();
    }
}
} // namespace lux::editor::gui
