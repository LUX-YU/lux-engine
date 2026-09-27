#pragma once

#include <lux/engine/ui/rendering/RenderFeature.hpp>

namespace lux::editor::ui::detail
{
    // A fixed CPU input. Null frame requests clear; this component is never persisted.
    struct UiFrame final
    {
        std::shared_ptr<const lux::ui::RenderFrame> frame;
    };
}
