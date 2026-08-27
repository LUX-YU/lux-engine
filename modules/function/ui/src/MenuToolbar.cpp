#include <lux/engine/ui/MenuToolbar.hpp>

#include <imgui.h>

#include <lux/engine/ui/CommandRouter.hpp>

namespace lux::ui
{
    namespace
    {
        void drawMenuItem(const MenuItem& item, CommandRouter& router)
        {
            if (item.kind() == EMenuItemKind::SEPARATOR)
            {
                ImGui::Separator();
                return;
            }
            if (item.kind() == EMenuItemKind::SUBMENU)
            {
                if (!ImGui::BeginMenu(item.label().data()))
                    return;
                for (const auto& child : item.children())
                    drawMenuItem(child, router);
                ImGui::EndMenu();
                return;
            }

            const auto state = router.state(item.command());
            if (!state.found)
                return;
            auto label = item.label();
            if (label.empty())
                label = router.label(item.command());
            if (ImGui::MenuItem(label.data(), nullptr, state.checked, state.enabled))
            {
                static_cast<void>(router.invoke(item.command()));
            }
        }
    } // namespace

    void drawMenu(std::span<const MenuItem> items, CommandRouter& router)
    {
        for (const auto& item : items)
            drawMenuItem(item, router);
    }

    void drawToolbar(std::span<const ToolbarItem> items, CommandRouter& router)
    {
        bool first = true;
        for (const auto& item : items)
        {
            if (!first)
                ImGui::SameLine();
            first = false;
            if (item.kind() == EToolbarItemKind::SEPARATOR)
            {
                ImGui::TextUnformatted("|");
                continue;
            }

            const auto state = router.state(item.command());
            if (!state.found)
                continue;
            const auto label = router.label(item.command());
            ImGui::BeginDisabled(!state.enabled);
            if (ImGui::Button(label.data()))
                static_cast<void>(router.invoke(item.command()));
            ImGui::EndDisabled();
        }
    }
} // namespace lux::ui
