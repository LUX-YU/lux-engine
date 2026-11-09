#pragma once

#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::render
{
    inline constexpr std::string_view kCameraInput = "lux.render.input.camera";

    // Column-major matrices; position/direction are world-space, direction is unit length.
    // This is semantic camera data, not a concatenation of implementation shader blocks.
    struct CameraInput final
    {
        TransformMatrix view = kIdentityTransform;
        TransformMatrix projection = kIdentityTransform;
        TransformMatrix inverse_view_projection = kIdentityTransform;
        std::array<float, 3> position{}, direction{0, 0, -1};
        float near_distance = 0.1F, far_distance = 1000;
        std::array<float, 4> viewport{0, 0, 1, 1}; // Normalized x/y/width/height, not pixel dimensions.
        std::array<float, 4> background{};
        float exposure = 1;
        bool operator==(const CameraInput&) const noexcept = default;
    };

} // namespace lux::render
