#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/meta/MetaDef.hpp>

#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

namespace lux::spatial
{
    /// Absolute position in a registry's 2D spatial frame. This is deliberately
    /// an ordinary value: memory paging, streaming cells and render origins are
    /// implementation details of their respective owners.
    struct LUX_CLASS() Position2D final
    {
        LUX_MEMBER(display_name=X)
        double x{0.0};
        LUX_MEMBER(display_name=Y)
        double y{0.0};

        friend bool operator==(const Position2D&, const Position2D&) = default;
    };

    /// Absolute position in a registry's 3D spatial frame.
    struct LUX_CLASS() Position3D final
    {
        LUX_MEMBER(display_name=X)
        double x{0.0};
        LUX_MEMBER(display_name=Y)
        double y{0.0};
        LUX_MEMBER(display_name=Z)
        double z{0.0};

        friend bool operator==(const Position3D&, const Position3D&) = default;
    };

    /// Integer coordinates identify grids/chunks/sections; they are never an
    /// alternative Transform representation.
    struct LUX_CLASS() GridCoord2i64 final
    {
        LUX_MEMBER(display_name=X)
        std::int64_t x{0};
        LUX_MEMBER(display_name=Y)
        std::int64_t y{0};

        friend bool operator==(const GridCoord2i64&, const GridCoord2i64&) = default;
        friend auto operator<=>(const GridCoord2i64&, const GridCoord2i64&) = default;
    };

    struct LUX_CLASS() GridCoord3i64 final
    {
        LUX_MEMBER(display_name=X)
        std::int64_t x{0};
        LUX_MEMBER(display_name=Y)
        std::int64_t y{0};
        LUX_MEMBER(display_name=Z)
        std::int64_t z{0};

        friend bool operator==(const GridCoord3i64&, const GridCoord3i64&) = default;
        friend auto operator<=>(const GridCoord3i64&, const GridCoord3i64&) = default;
    };

    [[nodiscard]] inline bool isFinite(const Position2D& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    [[nodiscard]] inline bool isFinite(const Position3D& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    [[nodiscard]] inline std::optional<std::array<float, 2u>> relativeFloat(
        const Position2D& position,
        const Position2D& origin,
        float maximum_extent) noexcept
    {
        if (!isFinite(position) || !isFinite(origin) ||
            !std::isfinite(maximum_extent) || maximum_extent < 0.0f)
        {
            return std::nullopt;
        }
        const double x = position.x - origin.x;
        const double y = position.y - origin.y;
        if (std::abs(x) > static_cast<double>(maximum_extent) ||
            std::abs(y) > static_cast<double>(maximum_extent))
        {
            return std::nullopt;
        }
        const float result_x = static_cast<float>(x);
        const float result_y = static_cast<float>(y);
        if (!std::isfinite(result_x) || !std::isfinite(result_y))
            return std::nullopt;
        return std::array<float, 2u>{result_x, result_y};
    }

    [[nodiscard]] inline std::optional<std::array<float, 3u>> relativeFloat(
        const Position3D& position,
        const Position3D& origin,
        float maximum_extent) noexcept
    {
        if (!isFinite(position) || !isFinite(origin) ||
            !std::isfinite(maximum_extent) || maximum_extent < 0.0f)
        {
            return std::nullopt;
        }
        const double x = position.x - origin.x;
        const double y = position.y - origin.y;
        const double z = position.z - origin.z;
        if (std::abs(x) > static_cast<double>(maximum_extent) ||
            std::abs(y) > static_cast<double>(maximum_extent) ||
            std::abs(z) > static_cast<double>(maximum_extent))
        {
            return std::nullopt;
        }
        const float result_x = static_cast<float>(x);
        const float result_y = static_cast<float>(y);
        const float result_z = static_cast<float>(z);
        if (!std::isfinite(result_x) || !std::isfinite(result_y) ||
            !std::isfinite(result_z))
        {
            return std::nullopt;
        }
        return std::array<float, 3u>{result_x, result_y, result_z};
    }
} // namespace lux::spatial

namespace lux::meta
{
    template <>
    inline constexpr bool is_reflected_value_v<lux::spatial::Position2D> = true;

    template <>
    inline constexpr bool is_reflected_value_v<lux::spatial::Position3D> = true;

    template <>
    inline constexpr bool is_reflected_value_v<lux::spatial::GridCoord2i64> = true;

    template <>
    inline constexpr bool is_reflected_value_v<lux::spatial::GridCoord3i64> = true;
}
