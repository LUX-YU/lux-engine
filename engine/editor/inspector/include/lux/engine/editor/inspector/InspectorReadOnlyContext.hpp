#pragma once

#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::inspector
{
    // Deliberately carries no EditorContext, writable Registry or history journal.
    struct InspectorReadOnlyContext final
    {
        ui::Frame& frame;
    };
}
