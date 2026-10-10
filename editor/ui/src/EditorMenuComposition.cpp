#include <lux/engine/editor/EditorMenuComposition.hpp>

#include <algorithm>

namespace lux::editor
{
    namespace
    {
        template <class T> using Result = cxx::expected<T, MenuCompositionFailure>;

        auto failure(EMenuCompositionError code, std::string_view id) noexcept
        {
            return cxx::unexpected(MenuCompositionFailure{code, std::string(id)});
        }

        template <class T, class Key, class Rank>
        Result<std::vector<const T*>> ordered(std::vector<const T*> input, Key key, Rank rank) noexcept
        {
            const auto count = input.size();
            std::vector<std::vector<std::size_t>> edges(count);
            std::vector<std::size_t> incoming(count);
            for (std::size_t index{}; index != count; ++index)
            {
                const auto add = [&](const auto& anchor, bool before) noexcept -> Result<void>
                {
                    if (!anchor.isValid())
                    {
                        return {};
                    }
                    const auto found =
                        std::ranges::find_if(input, [&](const T* other) { return key(*other) == anchor; });
                    if (found == input.end())
                    {
                        return failure(EMenuCompositionError::UNKNOWN_ANCHOR, anchor.name());
                    }
                    const auto target = static_cast<std::size_t>(found - input.begin());
                    const auto from = before ? index : target;
                    const auto to = before ? target : index;
                    if (std::ranges::find(edges[from], to) == edges[from].end())
                    {
                        edges[from].push_back(to);
                        ++incoming[to];
                    }
                    return {};
                };
                if (auto result = add(input[index]->order.before, true); !result)
                {
                    return cxx::unexpected(std::move(result.error()));
                }
                if (auto result = add(input[index]->order.after, false); !result)
                {
                    return cxx::unexpected(std::move(result.error()));
                }
            }
            std::vector<const T*> result;
            result.reserve(count);
            std::vector<bool> emitted(count);
            while (result.size() != count)
            {
                auto selected = count;
                for (std::size_t index{}; index != count; ++index)
                {
                    const bool is_ready = !emitted[index] && incoming[index] == 0;
                    if (!is_ready)
                    {
                        continue;
                    }
                    const auto less = [&](const T& lhs, const T& rhs) noexcept
                    {
                        if (rank(lhs) != rank(rhs))
                        {
                            return rank(lhs) < rank(rhs);
                        }
                        if (lhs.order.priority != rhs.order.priority)
                        {
                            return lhs.order.priority > rhs.order.priority;
                        }
                        return key(lhs).name() < key(rhs).name();
                    };
                    if (selected == count || less(*input[index], *input[selected]))
                    {
                        selected = index;
                    }
                }
                if (selected == count)
                {
                    return failure(EMenuCompositionError::CYCLIC_ORDER, key(*input.front()).name());
                }
                emitted[selected] = true;
                result.push_back(input[selected]);
                for (auto target : edges[selected])
                {
                    --incoming[target];
                }
            }
            return result;
        }

        template <class T, class Key> Result<void> unique(const std::vector<T>& values, Key key) noexcept
        {
            for (std::size_t index{}; index != values.size(); ++index)
            {
                const auto& id = key(values[index]);
                const bool is_invalid = !id.isValid() || id.name().find('\0') != std::string_view::npos;
                if (is_invalid)
                {
                    return failure(EMenuCompositionError::INVALID_ID, id.name());
                }
                for (std::size_t previous{}; previous != index; ++previous)
                {
                    if (key(values[previous]) == id)
                    {
                        return failure(EMenuCompositionError::DUPLICATE_ID, id.name());
                    }
                }
            }
            return {};
        }
    } // namespace

    MenuCompositionResult composeEditorMenu(const EditorMenuComposition& composition) noexcept
    {
        const auto id = [](const auto& value) noexcept -> const auto& { return value.id; };
        for (auto result :
             {unique(composition.actions, id), unique(composition.menus, id), unique(composition.groups, id)})
        {
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
        }
        const auto has_menu = [&](const ui::MenuId& menu) noexcept
        { return std::ranges::find(composition.menus, menu, &EditorMenu::id) != composition.menus.end(); };
        for (const auto& menu : composition.menus)
        {
            if (menu.parent.isValid() && !has_menu(menu.parent))
            {
                return failure(EMenuCompositionError::UNKNOWN_MENU, menu.parent.name());
            }
        }
        for (const auto& group : composition.groups)
        {
            if (!has_menu(group.menu))
            {
                return failure(EMenuCompositionError::UNKNOWN_MENU, group.menu.name());
            }
        }
        for (std::size_t index{}; index != composition.contributions.size(); ++index)
        {
            const auto& item = composition.contributions[index];
            if (!has_menu(item.menu))
            {
                return failure(EMenuCompositionError::UNKNOWN_MENU, item.menu.name());
            }
            const auto group = std::ranges::find(composition.groups, item.group, &EditorMenuGroup::id);
            const bool is_invalid_group = group == composition.groups.end() || group->menu != item.menu;
            if (is_invalid_group)
            {
                return failure(EMenuCompositionError::UNKNOWN_GROUP, item.group.name());
            }
            if (std::ranges::find(composition.actions, item.action, &ui::ActionDescriptor::id) ==
                composition.actions.end())
            {
                return failure(EMenuCompositionError::UNKNOWN_ACTION, item.action.name());
            }
            for (std::size_t previous{}; previous != index; ++previous)
            {
                const auto& other = composition.contributions[previous];
                const bool is_duplicate = other.menu == item.menu && other.action == item.action;
                if (is_duplicate)
                {
                    return failure(EMenuCompositionError::DUPLICATE_PLACEMENT, item.action.name());
                }
            }
        }

        std::size_t visited{};
        auto children = [&](auto&& self, const ui::MenuId& parent) noexcept -> Result<std::vector<ui::MenuNode>>
        {
            std::vector<const EditorMenu*> siblings;
            for (const auto& menu : composition.menus)
            {
                const bool is_child = parent.isValid() ? menu.parent == parent : !menu.parent.isValid();
                if (is_child)
                {
                    siblings.push_back(&menu);
                }
            }
            const auto zero = [](const auto&) noexcept { return 0; };
            auto menus = ordered(std::move(siblings), id, zero);
            if (!menus)
            {
                return cxx::unexpected(std::move(menus.error()));
            }
            std::vector<ui::MenuNode> nodes;
            for (const auto* menu : *menus)
            {
                ++visited;
                ui::MenuNode node{menu->id, menu->label, {}};
                auto nested = self(self, menu->id);
                if (!nested)
                {
                    return cxx::unexpected(std::move(nested.error()));
                }
                for (auto& child : *nested)
                {
                    node.children.emplace_back(std::move(child));
                }
                std::vector<const EditorMenuGroup*> members;
                for (const auto& group : composition.groups)
                {
                    if (group.menu == menu->id)
                    {
                        members.push_back(&group);
                    }
                }
                auto groups = ordered(std::move(members), id, zero);
                if (!groups)
                {
                    return cxx::unexpected(std::move(groups.error()));
                }
                std::vector<const MenuContribution*> placements;
                for (const auto& item : composition.contributions)
                {
                    if (item.menu == menu->id)
                    {
                        placements.push_back(&item);
                    }
                }
                const auto rank = [&](const MenuContribution& item) noexcept
                {
                    return std::ranges::find_if(*groups, [&](const auto* group) { return group->id == item.group; }) -
                           groups->begin();
                };
                auto actions = ordered(
                    std::move(placements),
                    [](const MenuContribution& item) noexcept -> const auto& { return item.action; },
                    rank
                );
                if (!actions)
                {
                    return cxx::unexpected(std::move(actions.error()));
                }
                MenuGroupId previous;
                for (const auto* item : *actions)
                {
                    if (previous != item->group && !node.children.empty())
                    {
                        node.children.emplace_back(ui::MenuSeparator{});
                    }
                    node.children.emplace_back(ui::MenuAction{item->action});
                    previous = item->group;
                }
                nodes.push_back(std::move(node));
            }
            return nodes;
        };
        auto menus = children(children, {});
        if (!menus)
        {
            return cxx::unexpected(std::move(menus.error()));
        }
        if (visited != composition.menus.size())
        {
            return failure(EMenuCompositionError::CYCLIC_ORDER, "menu parent");
        }
        auto actions = composition.actions;
        std::ranges::sort(actions, {}, [](const ui::ActionDescriptor& action) noexcept { return action.id.name(); });
        return ui::MenuDefinition{std::move(actions), std::move(*menus)};
    }
} // namespace lux::editor
