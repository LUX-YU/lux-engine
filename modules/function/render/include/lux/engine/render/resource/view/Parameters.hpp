#pragma once

#include <lux/engine/render/resource/transform/Data.hpp>

namespace lux::render
{
    // Complete shader view block. Matrices are column-major and supplied by the caller.
    // Position/direction are world-space, and viewport is normalized x/y/width/height.
    struct alignas(16) ViewGpuParameters final
    {
        TransformMatrix view = kIdentityTransform;
        TransformMatrix projection = kIdentityTransform;
        TransformMatrix inverse_view_projection = kIdentityTransform;
        std::array<float, 4> position_near{0, 0, 0, 0.1F};
        std::array<float, 4> direction_far{0, 0, -1, 1000};
        std::array<float, 4> viewport{0, 0, 1, 1};
        std::array<float, 4> background{};

        bool operator==(const ViewGpuParameters&) const noexcept = default;
    };
} // namespace lux::render
