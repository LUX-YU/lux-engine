#pragma once

#include <cmath>

namespace lux::math
{
    /// Absolute position in a registry's 2D spatial frame. Streaming cells,
    /// memory paging and render origins remain concerns of their owners.
    struct Position2d final
    {
        double x{0.0};
        double y{0.0};

        friend bool operator==(const Position2d&, const Position2d&) = default;
    };

    /// Absolute position in a registry's 3D spatial frame.
    struct Position3d final
    {
        double x{0.0};
        double y{0.0};
        double z{0.0};

        friend bool operator==(const Position3d&, const Position3d&) = default;
    };

    [[nodiscard]] inline bool isFinite(const Position2d& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    [[nodiscard]] inline bool isFinite(const Position3d& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }
} // namespace lux::math
