#include <cmath>
#include <lux/engine/render/core/RasterViewport.hpp>

namespace lux::render
{
    RenderResult<void> RasterViewport::validate() const noexcept
    {
        const bool is_finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && std::isfinite(height) &&
                               std::isfinite(min_depth) && std::isfinite(max_depth);
        const bool is_invalid_rectangle =
            x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > 1 || y + height > 1;
        const bool is_invalid_depth = min_depth < 0 || max_depth > 1 || min_depth > max_depth;
        const bool is_invalid_viewport = !is_finite || is_invalid_rectangle || is_invalid_depth;
        if (is_invalid_viewport)
        {
            return cxx::unexpected(RenderError{kInvalidViewport});
        }
        return {};
    }
} // namespace lux::render
