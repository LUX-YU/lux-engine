#pragma once

#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Shortcut.hpp>
#include <string>
#include <vector>

namespace lux::ui
{
    class Pane;
    class Element;

    struct MenuItem final
    {
        CommandId command;
        std::string label;
        std::string shortcut_label;
        Shortcut shortcut;
        std::vector<MenuItem> children;
    };

    enum class EMenuAction : std::uint8_t
    {
        OPEN,
        COMMAND,
        CLOSE
    };

    // Synchronous host request. Targets are borrowed only for this dispatch.
    // The host retains its own business identity before accepting an invocation.
    struct MenuRequest final
    {
        EMenuAction action{EMenuAction::OPEN};
        Pane* pane{};
        Element* element{};
        Command command;
    };
} // namespace lux::ui
