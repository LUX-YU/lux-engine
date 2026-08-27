#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/Command.hpp>

namespace lux::ui
{
    enum class EMenuItemKind
    {
        COMMAND,
        SUBMENU,
        SEPARATOR
    };

    class MenuItem final
    {
    public:
        [[nodiscard]] static MenuItem command(CommandHandle command, std::string label = {})
        {
            return MenuItem{EMenuItemKind::COMMAND, std::move(label), command, {}};
        }

        [[nodiscard]] static MenuItem submenu(std::string label, std::vector<MenuItem> children)
        {
            return MenuItem{EMenuItemKind::SUBMENU, std::move(label), {}, std::move(children)};
        }

        [[nodiscard]] static MenuItem separator()
        {
            return MenuItem{EMenuItemKind::SEPARATOR, {}, {}, {}};
        }

        [[nodiscard]] EMenuItemKind kind() const noexcept
        {
            return kind_;
        }
        [[nodiscard]] std::string_view label() const noexcept
        {
            return label_;
        }
        [[nodiscard]] CommandHandle command() const noexcept
        {
            return command_;
        }
        [[nodiscard]] std::span<const MenuItem> children() const noexcept
        {
            return children_;
        }

    private:
        MenuItem(EMenuItemKind kind, std::string label, CommandHandle command, std::vector<MenuItem> children)
            : kind_(kind), label_(std::move(label)), command_(command), children_(std::move(children))
        {
        }

        EMenuItemKind kind_;
        std::string label_;
        CommandHandle command_;
        std::vector<MenuItem> children_;
    };

    enum class EToolbarItemKind
    {
        COMMAND,
        SEPARATOR
    };

    class ToolbarItem final
    {
    public:
        [[nodiscard]] static ToolbarItem command(CommandHandle command)
        {
            return ToolbarItem{EToolbarItemKind::COMMAND, command};
        }

        [[nodiscard]] static ToolbarItem separator()
        {
            return ToolbarItem{EToolbarItemKind::SEPARATOR, {}};
        }

        [[nodiscard]] EToolbarItemKind kind() const noexcept
        {
            return kind_;
        }
        [[nodiscard]] CommandHandle command() const noexcept
        {
            return command_;
        }

    private:
        ToolbarItem(EToolbarItemKind kind, CommandHandle command) noexcept : kind_(kind), command_(command)
        {
        }

        EToolbarItemKind kind_;
        CommandHandle command_;
    };

    class CommandRouter;

    LUX_FUNCTION_PUBLIC void drawMenu(std::span<const MenuItem> items, CommandRouter& router);

    LUX_FUNCTION_PUBLIC void drawToolbar(std::span<const ToolbarItem> items, CommandRouter& router);
} // namespace lux::ui
