#pragma once

#include <lux/engine/math/Position.hpp>

#include <array>
#include <cmath>
#include <optional>

namespace lux::math
{
    [[nodiscard]] inline std::optional<std::array<float, 2u>>
    relativeFloat(const Position2d& position, const Position2d& origin, float maximum_extent) noexcept
    {
        const bool is_invalid_position = !isFinite(position) || !isFinite(origin);
        const bool is_invalid_extent = !std::isfinite(maximum_extent) || maximum_extent < 0.0f;
        const bool is_invalid_input = is_invalid_position || is_invalid_extent;
        if (is_invalid_input)
        {
            return std::nullopt;
        }
        const double x = position.x - origin.x;
        const double y = position.y - origin.y;
        const bool is_outside_x = std::abs(x) > static_cast<double>(maximum_extent);
        const bool is_outside_y = std::abs(y) > static_cast<double>(maximum_extent);
        if (is_outside_x || is_outside_y)
        {
            return std::nullopt;
        }
        const float result_x = static_cast<float>(x);
        const float result_y = static_cast<float>(y);
        if (!std::isfinite(result_x) || !std::isfinite(result_y))
            return std::nullopt;
        return std::array<float, 2u>{result_x, result_y};
    }

    [[nodiscard]] inline std::optional<std::array<float, 3u>>
    relativeFloat(const Position3d& position, const Position3d& origin, float maximum_extent) noexcept
    {
        const bool is_invalid_position = !isFinite(position) || !isFinite(origin);
        const bool is_invalid_extent = !std::isfinite(maximum_extent) || maximum_extent < 0.0f;
        const bool is_invalid_input = is_invalid_position || is_invalid_extent;
        if (is_invalid_input)
        {
            return std::nullopt;
        }
        const double x = position.x - origin.x;
        const double y = position.y - origin.y;
        const double z = position.z - origin.z;
        const bool is_outside_x = std::abs(x) > static_cast<double>(maximum_extent);
        const bool is_outside_y = std::abs(y) > static_cast<double>(maximum_extent);
        const bool is_outside_z = std::abs(z) > static_cast<double>(maximum_extent);
        if (is_outside_x || is_outside_y || is_outside_z)
        {
            return std::nullopt;
        }
        const float result_x = static_cast<float>(x);
        const float result_y = static_cast<float>(y);
        const float result_z = static_cast<float>(z);
        const bool is_invalid_result = !std::isfinite(result_x) || !std::isfinite(result_y) ||
            !std::isfinite(result_z);
        if (is_invalid_result)
        {
            return std::nullopt;
        }
        return std::array<float, 3u>{result_x, result_y, result_z};
    }
} // namespace lux::math
