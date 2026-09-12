#include <lux/engine/editor/ui/detail/EditorEditMenu.hpp>
#include <imgui.h>

namespace lux::editor::ui::detail
{
    EditorEditMenu::EditorEditMenu(lux::object::ObjectDispatcherRef dispatcher, ActiveEditHistory &histories,
                                   lux::ui::CommandRouter &commands)
        : actions_(std::move(dispatcher), histories), commands_(commands)
    {
        const auto undo = commands.defineCommand({lux::ui::UiCommandId{"lux.menu.edit.undo"}, "Undo"});
        const auto redo = commands.defineCommand({lux::ui::UiCommandId{"lux.menu.edit.redo"}, "Redo"});
        if (!undo || !redo) return;
        undo_ = *undo;
        redo_ = *redo;
        auto first = commands.bindGlobal<&HistoryMenuActions::undo, &HistoryMenuActions::canUndo>(undo_, actions_);
        auto second = commands.bindGlobal<&HistoryMenuActions::redo, &HistoryMenuActions::canRedo>(redo_, actions_);
        if (!first || !second) return;
        undo_binding_ = std::move(*first);
        redo_binding_ = std::move(*second);
        valid_ = true;
    }
    void EditorEditMenu::draw()
    {
        bool shown{};
        // The existing top Pane reserves this area. No second input system or lower UI layout change.
        ImGui::BeginChild("##editor-edit-menu", ImVec2{125, 30}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar);
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Edit"))
            {
                shown = true;
                if (!open_) static_cast<void>(actions_.capture());
                const auto item = [&](lux::ui::CommandHandle command, const char *label, const char *shortcut) {
                    const auto state = commands_.state(command);
                    if (ImGui::MenuItem(label, shortcut, false, state.enabled))
                        static_cast<void>(commands_.invoke(command));
                };
                item(undo_, "Undo", "Ctrl+Z");
                item(redo_, "Redo", "Ctrl+Y");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::EndChild();
        if (!shown && open_) static_cast<void>(actions_.cancel());
        open_ = shown;
    }
}
