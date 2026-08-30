#pragma once

#include <lux/engine/math/Grid.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/spatial/spatial3d/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lux::spatial
{
    struct Spatial3DPartitionIndexEntry final
    {
        math::GridCoord3i64 coordinate;
        partition::PartitionOrdinal partition;

        friend bool operator==(const Spatial3DPartitionIndexEntry&, const Spatial3DPartitionIndexEntry&) = default;
    };

    /** Half-open absolute bounds: [minimum, maximum). */
    struct Spatial3DQueryBounds final
    {
        math::Position3d minimum;
        math::Position3d maximum;
    };

    enum class ESpatial3DPartitionIndexError : std::uint8_t
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

    struct Spatial3DPartitionIndexFailure final
    {
        ESpatial3DPartitionIndexError code{ESpatial3DPartitionIndexError::EMPTY_INDEX};
        math::GridCoord3i64 coordinate;
        partition::PartitionOrdinal partition;
        std::size_t required_capacity{};
    };

    class LUX_ENGINE_SPATIAL3D_INDEX_PUBLIC Spatial3DPartitionIndex final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<Spatial3DPartitionIndex, Spatial3DPartitionIndexFailure> create(
            math::Position3d grid_origin,
            double cell_world_size,
            std::vector<Spatial3DPartitionIndexEntry> entries
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<math::GridCoord3i64, Spatial3DPartitionIndexFailure>
        coordinate(math::Position3d position) const noexcept;

        [[nodiscard]] const partition::PartitionOrdinal* find(math::GridCoord3i64 coordinate) const noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::optional<partition::PartitionOrdinal>,
            Spatial3DPartitionIndexFailure
        > find(math::Position3d position) const noexcept;

        [[nodiscard]] lux::cxx::expected<std::size_t, Spatial3DPartitionIndexFailure> query(
            const Spatial3DQueryBounds& bounds,
            std::span<partition::PartitionOrdinal> output
        ) const noexcept;

        [[nodiscard]] math::Position3d gridOrigin() const noexcept;
        [[nodiscard]] double cellWorldSize() const noexcept;
        [[nodiscard]] std::span<const Spatial3DPartitionIndexEntry> entries() const noexcept;

    private:
        Spatial3DPartitionIndex(
            math::Position3d grid_origin,
            double cell_world_size,
            std::vector<Spatial3DPartitionIndexEntry> entries
        ) noexcept;

        math::Position3d grid_origin_;
        double cell_world_size_{};
        std::vector<Spatial3DPartitionIndexEntry> entries_;
    };
}
