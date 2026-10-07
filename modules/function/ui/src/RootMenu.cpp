#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    void Root::setMenu(std::vector<MenuItem> menu)
    {
        checkContentChange();
        impl_->menu_state.items = std::move(menu);
    }
    std::span<const MenuItem> Root::menu() const noexcept
    {
        requireOwner();
        return impl_->menu_state.items;
    }
    bool Root::menuTargets(const Element& element) const noexcept
    {
        requireOwner();
        return impl_->menu_state.open && impl_->menu_state.element == &element;
    }
    void Root::Impl::menuCommand(Root& root, Command& command) noexcept
    {
        object::LuxObject* target =
            menu_state.element ? static_cast<object::LuxObject*>(menu_state.element) : menu_state.pane;
        if (!target)
        {
            return;
        }
        if (command.phase == ECommandPhase::QUERY)
        {
            static_cast<void>(object::routeEvent(*target, root, command));
        }
        else
        {
            menu_state.calls.push_back(
                {store(root, *menu_state.pane, *target), CommandId{std::string(command.id.name())}}
            );
            command.result = ECommandDispatchResult::EXECUTED;
        }
    }
    void Root::Impl::drawMenuItems(Root& root, std::span<const MenuItem> items) noexcept
    {
        for (const auto& item : items)
        {
            const auto* label = item.label.empty() ? "" : item.label.data();
            if (!item.children.empty())
            {
                if (ImGui::BeginMenu(label))
                {
                    drawMenuItems(root, item.children);
                    ImGui::EndMenu();
                }
                continue;
            }
            if (!item.command.isValid())
            {
                if (item.label.empty())
                {
                    ImGui::Separator();
                }
                else
                {
                    ImGui::TextDisabled("%s", label);
                }
                continue;
            }
            Command command{item.command.view()};
            menuCommand(root, command);
            const auto* shortcut = item.shortcut_label.empty() ? "" : item.shortcut_label.data();
            if (ImGui::MenuItem(label, shortcut, command.checked, command.enabled))
            {
                command.phase = ECommandPhase::EXECUTE;
                menuCommand(root, command);
            }
        }
    }
    void Root::Impl::drawMenu(Root& root) noexcept
    {
        menu_state.height = 0;
        bool opened{};
        if (!menu_state.items.empty() && ImGui::BeginMainMenuBar())
        {
            menu_state.height = ImGui::GetWindowHeight();
            for (const auto& item : menu_state.items)
            {
                if (ImGui::BeginMenu((item.label.empty() ? "" : item.label.data()), !focus_state.modal))
                {
                    opened = true;
                    if (!menu_state.open)
                    {
                        menu_state.pane = focus_state.focused;
                        menu_state.element = focus_state.focused_element;
                        menu_state.open = true;
                    }
                    drawMenuItems(root, item.children);
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMainMenuBar();
        }
        if (menu_state.open && !opened)
        {
            menu_state.pane = nullptr;
            menu_state.element = nullptr;
            menu_state.open = false;
        }
    }
    bool Root::Impl::shortcut(Root& root, const Key& key) noexcept
    {
        if (!key.down)
        {
            return false;
        }
        const bool control = (input_state.modifiers & ImGuiMod_Ctrl) != 0;
        const bool shift = (input_state.modifiers & ImGuiMod_Shift) != 0;
        const bool alt = (input_state.modifiers & ImGuiMod_Alt) != 0;
        const auto find = [&](auto&& self, std::span<const MenuItem> items) -> const MenuItem*
        {
            for (const auto& item : items)
            {
                const auto& binding = item.shortcut;
                const bool matches = binding.key == key.key && binding.control == control && binding.shift == shift &&
                                     binding.alt == alt;
                if (item.command.isValid() && matches)
                {
                    return &item;
                }
                if (const auto* nested = self(self, item.children))
                {
                    return nested;
                }
            }
            return nullptr;
        };
        const auto* item = find(find, menu_state.items);
        if (!item)
        {
            return false;
        }
        menu_state.pane = focus_state.focused;
        menu_state.element = focus_state.focused_element;
        Command command{item->command.view()};
        menuCommand(root, command);
        if (command.enabled)
        {
            command.phase = ECommandPhase::EXECUTE;
            menuCommand(root, command);
        }
        return true;
    }

} // namespace lux::ui
