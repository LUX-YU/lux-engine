#pragma once

#include <lux/engine/math/Grid.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/spatial/spatial2d/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lux::spatial
{
    struct Spatial2DPartitionIndexEntry final
    {
        math::GridCoord2i64 coordinate;
        partition::PartitionOrdinal partition;

        friend bool operator==(const Spatial2DPartitionIndexEntry&, const Spatial2DPartitionIndexEntry&) = default;
    };

    /** Half-open absolute bounds: [minimum, maximum). */
    struct Spatial2DQueryBounds final
    {
        math::Position2d minimum;
        math::Position2d maximum;
    };

    enum class ESpatial2DPartitionIndexError : std::uint8_t
    {
        EMPTY_INDEX,
        INVALID_GRID,
        INVALID_POSITION,
        INVALID_BOUNDS,
        INVALID_PARTITION,
        DUPLICATE_COORDINATE,
        DUPLICATE_PARTITION,
        COORDINATE_OVERFLOW,
        OUTPUT_CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    struct Spatial2DPartitionIndexFailure final
    {
        ESpatial2DPartitionIndexError code{ESpatial2DPartitionIndexError::EMPTY_INDEX};
        math::GridCoord2i64 coordinate;
        partition::PartitionOrdinal partition;
        std::size_t required_capacity{};
    };

    class LUX_ENGINE_SPATIAL2D_INDEX_PUBLIC Spatial2DPartitionIndex final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<Spatial2DPartitionIndex, Spatial2DPartitionIndexFailure> create(
            math::Position2d grid_origin,
            double cell_world_size,
            std::vector<Spatial2DPartitionIndexEntry> entries
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<math::GridCoord2i64, Spatial2DPartitionIndexFailure>
        coordinate(math::Position2d position) const noexcept;

        [[nodiscard]] const partition::PartitionOrdinal* find(math::GridCoord2i64 coordinate) const noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::optional<partition::PartitionOrdinal>,
            Spatial2DPartitionIndexFailure
        > find(math::Position2d position) const noexcept;

        [[nodiscard]] lux::cxx::expected<std::size_t, Spatial2DPartitionIndexFailure> query(
            const Spatial2DQueryBounds& bounds,
            std::span<partition::PartitionOrdinal> output
        ) const noexcept;

        [[nodiscard]] math::Position2d gridOrigin() const noexcept;
        [[nodiscard]] double cellWorldSize() const noexcept;
        [[nodiscard]] std::span<const Spatial2DPartitionIndexEntry> entries() const noexcept;

    private:
        Spatial2DPartitionIndex(
            math::Position2d grid_origin,
            double cell_world_size,
            std::vector<Spatial2DPartitionIndexEntry> entries
        ) noexcept;

        math::Position2d grid_origin_;
        double cell_world_size_{};
        std::vector<Spatial2DPartitionIndexEntry> entries_;
    };
}
