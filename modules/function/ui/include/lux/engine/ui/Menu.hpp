#pragma once

#include <cstdint>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Shortcut.hpp>
#include <string>
#include <variant>
#include <vector>

namespace lux::ui
{
    // Presentation defaults only. Enabled/checked and execution remain routed Command facts.
    struct ActionDescriptor final
    {
        CommandId id;
        std::string label;
        std::string shortcut_label;
        Shortcut shortcut;
        bool checkable{};
    };

    struct MenuAction final
    {
        CommandId action;
    };

    struct MenuSeparator final
    {
    };

    struct MenuNode;
    using VMenuEntry = std::variant<MenuAction, MenuSeparator, MenuNode>;

    struct MenuNode final
    {
        MenuId id;
        std::string label;
        std::vector<VMenuEntry> children;
    };

    // Owned cold input. Actions may be used in multiple menus or only by a shortcut.
    struct MenuDefinition final
    {
        std::vector<ActionDescriptor> actions;
        std::vector<MenuNode> menus;
    };

    enum class EMenuError : std::uint8_t
    {
        WRONG_THREAD,
        BUSY,
        CLOSED,
        INVALID_ACTION,
        DUPLICATE_ACTION,
        DUPLICATE_SHORTCUT,
        INVALID_MENU,
        DUPLICATE_MENU,
        UNKNOWN_ACTION
    };

} // namespace lux::ui
