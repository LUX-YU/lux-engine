#include <lux/engine/render/core/Camera.hpp>
#include <algorithm>
#include <cmath>

namespace lux::render
{
    RenderResult<void> CameraViewport::validate() const noexcept
    {
        const bool is_finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
            std::isfinite(height) && std::isfinite(min_depth) && std::isfinite(max_depth);
        const bool is_invalid_rectangle = x < 0 || y < 0 || width <= 0 || height <= 0 ||
            x + width > 1 || y + height > 1;
        const bool is_invalid_depth = min_depth < 0 || max_depth > 1 || min_depth > max_depth;
        if (!is_finite || is_invalid_rectangle || is_invalid_depth)
            return cxx::unexpected(RenderError{kInvalidCamera});
        return {};
    }

    RenderResult<void> CameraState::validate() const noexcept
    {
        const auto finite = [](float value) noexcept { return std::isfinite(value); };
        if (!std::ranges::all_of(view, finite) || !std::ranges::all_of(projection, finite))
            return cxx::unexpected(RenderError{kInvalidCamera});
        return viewport.validate();
    }
}
