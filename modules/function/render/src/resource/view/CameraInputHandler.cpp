#include <lux/engine/render/resource/view/CameraInputHandler.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <algorithm>
#include <cmath>

namespace lux::render::detail
{
    RenderResult<void> CameraInputHandler::apply(FrameViewTarget target, CameraInput input) noexcept
    {
        const RasterViewport viewport{input.viewport[0], input.viewport[1], input.viewport[2], input.viewport[3]};
        if (auto valid = viewport.validate(); !valid) return valid;
        const auto finite = [](const auto& values) noexcept
        { return std::ranges::all_of(values, [](float value) noexcept { return std::isfinite(value); }); };
        const bool has_invalid_values = !finite(input.view) || !finite(input.projection) ||
            !finite(input.inverse_view_projection) || !finite(input.position) || !finite(input.direction) ||
            !finite(input.background) || !std::isfinite(input.exposure) || input.exposure < 0;
        const bool has_invalid_planes = !std::isfinite(input.near_distance) || !std::isfinite(input.far_distance) ||
            input.near_distance <= 0 || input.far_distance <= input.near_distance;
        const auto& direction = input.direction;
        const auto length = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
        const bool has_invalid_direction = !std::isfinite(length) || std::abs(length - 1.F) > 0.001F;
        const bool is_invalid_camera = has_invalid_values || has_invalid_planes || has_invalid_direction;
        if (is_invalid_camera) return cxx::unexpected(RenderError{kFrameContract});
        if (target.camera != input)
        {
            target.camera = input;
            ++target.revision;
        }
        return {};
    }
} // namespace lux::render::detail
