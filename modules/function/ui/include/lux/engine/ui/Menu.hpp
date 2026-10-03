#pragma once

#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/InputEvent.hpp>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::ui
{
    class Pane;
    class Element;

    struct Shortcut final
    {
        EKey key{EKey::NONE};
        bool control{}, shift{}, alt{};
    };

    struct MenuItem final
    {
        CommandIdView command;
        // Terminated text borrowed from Root's menu source (or static literals).
        std::string_view label;
        std::string_view shortcut_label;
        Shortcut shortcut;
        std::vector<MenuItem> children;
        std::size_t index{static_cast<std::size_t>(-1)};
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
        // Borrowed only for this synchronous dispatch. index is local to this exact source.
        const void* source{};
        std::size_t index{static_cast<std::size_t>(-1)};
    };
}
