#include <lux/engine/ui_next/MenuToolbar.hpp>

#include <imgui.h>

#include <lux/engine/ui_next/CommandRouter.hpp>

namespace lux::ui
{
    namespace
    {
        void drawMenuItem(const MenuItem& item, CommandRouter& router)
        {
            if (item.kind == EMenuItemKind::SEPARATOR)
            {
                ImGui::Separator();
                return;
            }
            if (item.kind == EMenuItemKind::SUBMENU)
            {
                if (!ImGui::BeginMenu(item.label.c_str()))
                    return;
                for (const auto& child : item.children)
                    drawMenuItem(child, router);
                ImGui::EndMenu();
                return;
            }

            const auto state = router.state(item.presentation.command);
            const auto* command = router.command(item.presentation.command);
            const auto label = item.label.empty() && command ? command->label.c_str()
                                                             : item.label.c_str();
            if (ImGui::MenuItem(
                    label,
                    nullptr,
                    state.checked,
                    state.found && state.enabled
                ))
            {
                static_cast<void>(router.invoke(item.presentation.command));
            }
        }
    } // namespace

    void drawMenu(const MenuModel& model, CommandRouter& router)
    {
        for (const auto& item : model.items)
            drawMenuItem(item, router);
    }

    void drawToolbar(const ToolbarModel& model, CommandRouter& router)
    {
        bool first = true;
        for (const auto& item : model.items)
        {
            if (!first)
                ImGui::SameLine();
            first = false;
            if (item.kind == EToolbarItemKind::SEPARATOR)
            {
                ImGui::TextUnformatted("|");
                continue;
            }
            const auto state = router.state(item.presentation.command);
            const auto* command = router.command(item.presentation.command);
            if (!command)
                continue;
            ImGui::BeginDisabled(!state.enabled);
            if (ImGui::Button(command->label.c_str()))
                static_cast<void>(router.invoke(item.presentation.command));
            ImGui::EndDisabled();
        }
    }
} // namespace lux::ui
