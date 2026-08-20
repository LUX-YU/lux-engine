#pragma once

#include <lux/engine/math/Position.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace lux::ecs::detail
{
    [[nodiscard]] inline std::optional<lux::math::Position2d>
    sparseCanvasCellPosition(
        const lux::math::Position2d& origin,
        std::int64_t x,
        std::int64_t y,
        double extent,
        double local_offset = 0.0) noexcept
    {
        if (!lux::math::isFinite(origin) || !std::isfinite(extent) ||
            !std::isfinite(local_offset) || !(extent > 0.0))
        {
            return std::nullopt;
        }
        const auto result_x = static_cast<long double>(origin.x) +
            static_cast<long double>(x) * extent + local_offset;
        const auto result_y = static_cast<long double>(origin.y) +
            static_cast<long double>(y) * extent + local_offset;
        constexpr auto limit = static_cast<long double>(
            std::numeric_limits<double>::max());
        if (result_x < -limit || result_x > limit ||
            result_y < -limit || result_y > limit)
        {
            return std::nullopt;
        }
        const lux::math::Position2d result{
            static_cast<double>(result_x),
            static_cast<double>(result_y)};
        return lux::math::isFinite(result)
            ? std::optional<lux::math::Position2d>{result}
            : std::nullopt;
    }
} // namespace lux::ecs::detail
