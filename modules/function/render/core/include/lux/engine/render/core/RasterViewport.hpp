#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    // Normalized Target coordinates; fractions retain their meaning after Target resize.
    struct RasterViewport final
    {
        float x = 0, y = 0, width = 1, height = 1, min_depth = 0, max_depth = 1;
        [[nodiscard]] RenderResult<void> validate() const noexcept;
        bool operator==(const RasterViewport&) const noexcept = default;
    };
} // namespace lux::render
