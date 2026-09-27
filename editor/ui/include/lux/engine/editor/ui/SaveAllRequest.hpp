#pragma once

#include <lux/engine/editor/EditorError.hpp>

namespace lux::editor::ui
{
    // A fixed batch asks each owner once to start, then queries its retained outcome.
    struct SaveAllRequest final
    {
        bool start{};
        bool applicable{};
        bool pending{};
        EditorResult<void> result;
    };
}
