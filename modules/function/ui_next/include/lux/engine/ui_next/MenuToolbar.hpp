#pragma once

#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui_next/Command.hpp>

namespace lux::ui
{
    enum class EMenuItemKind
    {
        COMMAND,
        SUBMENU,
        SEPARATOR
    };

    struct MenuItem final
    {
        EMenuItemKind kind{EMenuItemKind::COMMAND};
        std::string label;
        CommandPresentation presentation;
        std::vector<MenuItem> children;
    };

    struct MenuModel final
    {
        std::vector<MenuItem> items;
    };

    enum class EToolbarItemKind
    {
        COMMAND,
        SEPARATOR
    };

    struct ToolbarItem final
    {
        EToolbarItemKind kind{EToolbarItemKind::COMMAND};
        CommandPresentation presentation;
    };

    struct ToolbarModel final
    {
        std::vector<ToolbarItem> items;
    };

    class CommandRouter;

    LUX_FUNCTION_PUBLIC void drawMenu(const MenuModel& model, CommandRouter& router);
    LUX_FUNCTION_PUBLIC void
    drawToolbar(const ToolbarModel& model, CommandRouter& router);
} // namespace lux::ui
