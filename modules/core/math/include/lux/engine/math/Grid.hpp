#pragma once

#include <compare>
#include <cstdint>

namespace lux::math
{
    /// Integer coordinates identify grids, chunks and sections. They are not
    /// an alternative transform representation.
    struct GridCoord2i64 final
    {
        std::int64_t x{0};
        std::int64_t y{0};

        friend bool operator==(const GridCoord2i64&, const GridCoord2i64&) = default;
        friend auto operator<=>(const GridCoord2i64&, const GridCoord2i64&) = default;
    };

    struct GridCoord3i64 final
    {
        std::int64_t x{0};
        std::int64_t y{0};
        std::int64_t z{0};

        friend bool operator==(const GridCoord3i64&, const GridCoord3i64&) = default;
        friend auto operator<=>(const GridCoord3i64&, const GridCoord3i64&) = default;
    };
} // namespace lux::math
