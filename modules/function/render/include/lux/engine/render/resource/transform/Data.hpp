#pragma once

#include <lux/engine/render/resource/ResourceId.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace lux::render
{
    struct TransformTag;
    using TransformId = ResourceId<TransformTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    [[nodiscard]] inline bool isFiniteTransform(const TransformMatrix& value) noexcept
    {
        return std::ranges::all_of(value, [](float element) noexcept { return std::isfinite(element); });
    }

    struct TransformRecordUpdate final
    {
        TransformId transform;
        TransformMatrix matrix = kIdentityTransform;
    };
} // namespace lux::render
