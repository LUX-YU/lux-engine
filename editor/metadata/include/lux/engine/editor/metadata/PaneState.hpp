#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <string>

namespace lux::editor
{
    struct PaneState final
    {
        lux::ui::PaneTypeId type;
        lux::ui::PaneId id;
        bool visible{true};
        std::string payload;
    };

    struct FinishEditingRequest final
    {
        EditorResult<void> result;
    };
}
