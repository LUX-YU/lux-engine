#pragma once

#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Shortcut.hpp>
#include <string>
#include <vector>

namespace lux::ui
{
    struct MenuItem final
    {
        CommandId command;
        std::string label;
        std::string shortcut_label;
        Shortcut shortcut;
        std::vector<MenuItem> children;
    };

} // namespace lux::ui
