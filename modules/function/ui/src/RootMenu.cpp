#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    cxx::expected<void, EMenuError> Root::setMenu(MenuDefinition menu) noexcept
    {
        if (auto ready = checkStructureSafe(); !ready)
        {
            if (ready.error() == EPaneError::WRONG_THREAD)
            {
                return cxx::unexpected(EMenuError::WRONG_THREAD);
            }
            return cxx::unexpected(ready.error() == EPaneError::CLOSED ? EMenuError::CLOSED : EMenuError::BUSY);
        }
        auto definition = std::make_unique<const MenuDefinition>(std::move(menu));
        const auto& actions = definition->actions;
        for (std::size_t index{}; index < actions.size(); ++index)
        {
            const auto& action = actions[index];
            const bool is_invalid_action =
                !action.id.isValid() || action.id.name().find('\0') != std::string_view::npos;
            if (is_invalid_action)
            {
                return cxx::unexpected(EMenuError::INVALID_ACTION);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (actions[previous].id == action.id)
                {
                    return cxx::unexpected(EMenuError::DUPLICATE_ACTION);
                }
                const bool is_duplicate_shortcut =
                    action.shortcut.key != EKey::NONE && actions[previous].shortcut == action.shortcut;
                if (is_duplicate_shortcut)
                {
                    return cxx::unexpected(EMenuError::DUPLICATE_SHORTCUT);
                }
            }
        }
        using BoundNode = Impl::MenuState::BoundNode;
        std::vector<const MenuNode*> seen;
        auto resolve = [&](auto&& self, const MenuNode& node) noexcept -> cxx::expected<BoundNode, EMenuError>
        {
            const bool is_invalid_menu = !node.id.isValid() || node.id.name().find('\0') != std::string_view::npos;
            if (is_invalid_menu)
            {
                return cxx::unexpected(EMenuError::INVALID_MENU);
            }
            for (const auto* previous : seen)
            {
                if (previous->id == node.id)
                {
                    return cxx::unexpected(EMenuError::DUPLICATE_MENU);
                }
            }
            seen.push_back(&node);
            BoundNode bound{&node, {}};
            bound.children.reserve(node.children.size());
            for (const auto& entry : node.children)
            {
                if (const auto* reference = std::get_if<MenuAction>(&entry))
                {
                    const auto action = std::ranges::find(actions, reference->action, &ActionDescriptor::id);
                    if (action == actions.end())
                    {
                        return cxx::unexpected(EMenuError::UNKNOWN_ACTION);
                    }
                    bound.children.emplace_back(static_cast<std::size_t>(action - actions.begin()));
                }
                else if (const auto* child = std::get_if<MenuNode>(&entry))
                {
                    auto result = self(self, *child);
                    if (!result)
                    {
                        return cxx::unexpected(result.error());
                    }
                    bound.children.emplace_back(std::move(*result));
                }
                else
                {
                    bound.children.emplace_back(MenuSeparator{});
                }
            }
            return bound;
        };
        std::vector<BoundNode> menus;
        menus.reserve(definition->menus.size());
        for (const auto& node : definition->menus)
        {
            auto result = resolve(resolve, node);
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
            menus.push_back(std::move(*result));
        }
        auto& state = impl_->menu_state;
        state.menus = std::move(menus);
        state.definition = std::move(definition);
        state.open = false;
        state.pane = nullptr;
        state.element = nullptr;
        return {};
    }

    const MenuDefinition& Root::menu() const noexcept
    {
        requireOwner();
        return *impl_->menu_state.definition;
    }

    bool Root::menuTargets(const Element& element) const noexcept
    {
        requireOwner();
        return impl_->menu_state.open && impl_->menu_state.element == &element;
    }

    void Root::setCommandFallback(object::LuxObject* target) noexcept
    {
        requireOwner();
        impl_->command_fallback = target ? target->objectId() : object::ObjectId{};
    }

    void Root::Impl::routeCommand(Root& root, object::LuxObject* target, Command& command) noexcept
    {
        Root::beginCallbackBorrow(root);
        bool accepted = target && object::routeEvent(*target, root, command);
        if (!accepted)
        {
            auto fallback = object::ObjectRuntime::instance().resolve(command_fallback);
            if (fallback)
            {
                accepted = object::sendEvent(**fallback, command);
            }
        }
        if (!accepted)
        {
            command.enabled = false;
            command.result = ECommandDispatchResult::NOT_FOUND;
        }
        else if (command.phase == ECommandPhase::QUERY && !command.enabled)
        {
            command.result = ECommandDispatchResult::DISABLED;
        }
        else if (command.phase == ECommandPhase::EXECUTE && command.result == ECommandDispatchResult::NOT_FOUND)
        {
            command.result = ECommandDispatchResult::EXECUTED;
        }
        Root::endCallbackBorrow(root);
    }

    void Root::Impl::menuCommand(Root& root, Command& command) noexcept
    {
        object::LuxObject* target =
            menu_state.element ? static_cast<object::LuxObject*>(menu_state.element) : menu_state.pane;
        if (command.phase == ECommandPhase::QUERY)
        {
            routeCommand(root, target, command);
        }
        else
        {
            const auto stored = target ? store(root, *menu_state.pane, *target) : StoredTarget{};
            safe_point_state.pending.push_back({stored, CommandExecution{CommandId{std::string(command.id.name())}}});
            command.result = ECommandDispatchResult::EXECUTED;
        }
    }

    void Root::Impl::drawMenuItems(Root& root, std::span<const MenuState::VBoundEntry> items) noexcept
    {
        for (std::size_t index{}; index < items.size(); ++index)
        {
            const auto& item = items[index];
            ImGui::PushID(static_cast<int>(index));
            if (const auto* node = std::get_if<MenuState::BoundNode>(&item))
            {
                if (ImGui::BeginMenu(node->source->label.c_str()))
                {
                    drawMenuItems(root, node->children);
                    ImGui::EndMenu();
                }
            }
            else if (const auto* action_index = std::get_if<std::size_t>(&item))
            {
                const auto& action = menu_state.definition->actions[*action_index];
                Command command{action.id.view()};
                menuCommand(root, command);
                const bool checked = action.checkable && command.checked;
                if (ImGui::MenuItem(action.label.c_str(), action.shortcut_label.c_str(), checked, command.enabled))
                {
                    command.phase = ECommandPhase::EXECUTE;
                    menuCommand(root, command);
                }
            }
            else
            {
                ImGui::Separator();
            }
            ImGui::PopID();
        }
    }

    void Root::Impl::drawMenu(Root& root) noexcept
    {
        menu_state.height = 0;
        bool opened{};
        if (!menu_state.menus.empty() && ImGui::BeginMainMenuBar())
        {
            menu_state.height = ImGui::GetWindowHeight();
            for (const auto& item : menu_state.menus)
            {
                const auto name = item.source->id.name();
                ImGui::PushID(name.data(), name.data() + name.size());
                if (ImGui::BeginMenu(item.source->label.c_str(), !focus_state.modal))
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
                ImGui::PopID();
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
        const auto& actions = menu_state.definition->actions;
        const auto item = std::ranges::find_if(
            actions,
            [&](const ActionDescriptor& action) noexcept
            {
                const auto& binding = action.shortcut;
                return binding.key == key.key && binding.control == control && binding.shift == shift &&
                       binding.alt == alt;
            }
        );
        if (item == actions.end())
        {
            return false;
        }
        menu_state.pane = focus_state.focused;
        menu_state.element = focus_state.focused_element;
        Command command{item->id.view()};
        menuCommand(root, command);
        if (command.enabled)
        {
            command.phase = ECommandPhase::EXECUTE;
            menuCommand(root, command);
        }
        return true;
    }

} // namespace lux::ui
