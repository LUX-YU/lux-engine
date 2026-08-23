#pragma once

#include <string>

#include <lux/engine/ui_next/UiIds.hpp>

namespace lux::ui
{
    struct Command final
    {
        UiCommandId id;
        std::string label;
    };

    struct CommandPresentation final
    {
        UiCommandId command;
    };

    enum class ECommandDispatchResult
    {
        EXECUTED,
        DISABLED,
        NOT_FOUND
    };
}
