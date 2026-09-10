#pragma once
#include <lux/engine/ui/Geometry.hpp>
#include <cstdint>

namespace lux::ui
{
    // Value from the most recently successfully captured frame of this UISession.
    // Client top-left origin, +Y down, in FrameInfo::display_size logical units.
    // This is the active text widget's request, NOT evidence of OS composition.
    struct UiTextInputAnchor final
    {
        Vec2 caret;
        float line_height{};
        std::uint64_t frame{};
        bool valid{}, want_visible{};
    };
}
