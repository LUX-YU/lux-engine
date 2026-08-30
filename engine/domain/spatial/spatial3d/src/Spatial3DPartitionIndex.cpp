#include <lux/engine/spatial/Spatial3DPartitionIndex.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace lux::spatial
{
    namespace
    {
        [[nodiscard]] bool coordinateLess(
            const Spatial3DPartitionIndexEntry& left,
            const Spatial3DPartitionIndexEntry& right
        ) noexcept
        {
            return left.coordinate < right.coordinate;
        }

        [[nodiscard]] Spatial3DPartitionIndexFailure failure(
            ESpatial3DPartitionIndexError code,
            math::GridCoord3i64 coordinate = {},
            partition::PartitionOrdinal partition = {},
            std::size_t required_capacity = 0U
        ) noexcept
        {
            return {code, coordinate, partition, required_capacity};
        }

        [[nodiscard]] bool coordinateValue(
            double value,
            double origin,
            double cell_world_size,
            std::int64_t& result
        ) noexcept
        {
            if (!std::isfinite(value))
            {
                return false;
            }
            const long double relative = static_cast<long double>(value) - static_cast<long double>(origin);
            const long double coordinate = std::floor(relative / static_cast<long double>(cell_world_size));
            constexpr long double kMinimum = -9223372036854775808.0L;
            constexpr long double kLimit = 9223372036854775808.0L;
            if (coordinate < kMinimum || coordinate >= kLimit)
            {
                return false;
            }
            result = static_cast<std::int64_t>(coordinate);
            return true;
        }

        [[nodiscard]] bool axisCount(std::int64_t first, std::int64_t last, std::uint64_t& count) noexcept
        {
            if (last < first)
            {
                return false;
            }
            constexpr std::uint64_t kSign = std::uint64_t{1U} << 63U;
            const std::uint64_t ordered_first = static_cast<std::uint64_t>(first) ^ kSign;
            const std::uint64_t ordered_last = static_cast<std::uint64_t>(last) ^ kSign;
            const std::uint64_t difference = ordered_last - ordered_first;
            if (difference == std::numeric_limits<std::uint64_t>::max())
            {
                return false;
            }
            count = difference + 1U;
            return true;
        }

        [[nodiscard]] bool checkedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept
        {
            if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right)
            {
                return false;
            }
            result = left * right;
            return true;
        }
    }

    Spatial3DPartitionIndex::Spatial3DPartitionIndex(
        math::Position3d grid_origin,
        double cell_world_size,
        std::vector<Spatial3DPartitionIndexEntry> entries
    ) noexcept
        : grid_origin_(grid_origin), cell_world_size_(cell_world_size), entries_(std::move(entries))
    {
    }

    lux::cxx::expected<Spatial3DPartitionIndex, Spatial3DPartitionIndexFailure>
    Spatial3DPartitionIndex::create(
        math::Position3d grid_origin,
        double cell_world_size,
        std::vector<Spatial3DPartitionIndexEntry> entries
    ) noexcept
    {
        if (!math::isFinite(grid_origin) || !std::isfinite(cell_world_size) || !(cell_world_size > 0.0))
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::INVALID_GRID));
        }
        if (entries.empty())
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::EMPTY_INDEX));
        }

        try
        {
            for (const auto& entry : entries)
            {
                if (entry.partition.value == std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(failure(
                        ESpatial3DPartitionIndexError::INVALID_PARTITION,
                        entry.coordinate,
                        entry.partition
                    ));
                }
            }
            std::sort(entries.begin(), entries.end(), coordinateLess);
            const auto duplicate_coordinate = std::adjacent_find(
                entries.begin(),
                entries.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.coordinate == right.coordinate;
                }
            );
            if (duplicate_coordinate != entries.end())
            {
                return lux::cxx::unexpected(failure(
                    ESpatial3DPartitionIndexError::DUPLICATE_COORDINATE,
                    duplicate_coordinate->coordinate,
                    duplicate_coordinate->partition
                ));
            }

            auto by_partition = entries;
            std::sort(
                by_partition.begin(),
                by_partition.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.partition.value < right.partition.value;
                }
            );
            const auto duplicate_partition = std::adjacent_find(
                by_partition.begin(),
                by_partition.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.partition == right.partition;
                }
            );
            if (duplicate_partition != by_partition.end())
            {
                return lux::cxx::unexpected(failure(
                    ESpatial3DPartitionIndexError::DUPLICATE_PARTITION,
                    duplicate_partition->coordinate,
                    duplicate_partition->partition
                ));
            }
            return Spatial3DPartitionIndex(grid_origin, cell_world_size, std::move(entries));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<math::GridCoord3i64, Spatial3DPartitionIndexFailure>
    Spatial3DPartitionIndex::coordinate(math::Position3d position) const noexcept
    {
        if (!math::isFinite(position))
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::INVALID_POSITION));
        }
        math::GridCoord3i64 result;
        if (!coordinateValue(position.x, grid_origin_.x, cell_world_size_, result.x) ||
            !coordinateValue(position.y, grid_origin_.y, cell_world_size_, result.y) ||
            !coordinateValue(position.z, grid_origin_.z, cell_world_size_, result.z))
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::COORDINATE_OVERFLOW));
        }
        return result;
    }

    const partition::PartitionOrdinal* Spatial3DPartitionIndex::find(math::GridCoord3i64 coordinate) const noexcept
    {
        const Spatial3DPartitionIndexEntry key{coordinate, {}};
        const auto found = std::lower_bound(entries_.begin(), entries_.end(), key, coordinateLess);
        return found != entries_.end() && found->coordinate == coordinate ? &found->partition : nullptr;
    }

    lux::cxx::expected<std::optional<partition::PartitionOrdinal>, Spatial3DPartitionIndexFailure>
    Spatial3DPartitionIndex::find(math::Position3d position) const noexcept
    {
        auto cell = coordinate(position);
        if (!cell)
        {
            return lux::cxx::unexpected(cell.error());
        }
        const auto* found = find(*cell);
        return found == nullptr ? std::optional<partition::PartitionOrdinal>{}
                                : std::optional<partition::PartitionOrdinal>{*found};
    }

    lux::cxx::expected<std::size_t, Spatial3DPartitionIndexFailure> Spatial3DPartitionIndex::query(
        const Spatial3DQueryBounds& bounds,
        std::span<partition::PartitionOrdinal> output
    ) const noexcept
    {
        const bool is_finite = math::isFinite(bounds.minimum) && math::isFinite(bounds.maximum);
        const bool is_ordered = bounds.minimum.x < bounds.maximum.x &&
            bounds.minimum.y < bounds.maximum.y && bounds.minimum.z < bounds.maximum.z;
        if (!is_finite || !is_ordered)
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::INVALID_BOUNDS));
        }

        auto first = coordinate(bounds.minimum);
        const math::Position3d inclusive_maximum{
            std::nextafter(bounds.maximum.x, -std::numeric_limits<double>::infinity()),
            std::nextafter(bounds.maximum.y, -std::numeric_limits<double>::infinity()),
            std::nextafter(bounds.maximum.z, -std::numeric_limits<double>::infinity())
        };
        auto last = coordinate(inclusive_maximum);
        if (!first || !last)
        {
            return lux::cxx::unexpected(
                failure(ESpatial3DPartitionIndexError::COORDINATE_OVERFLOW)
            );
        }

        std::uint64_t x_count{};
        std::uint64_t y_count{};
        std::uint64_t z_count{};
        std::uint64_t plane_count{};
        std::uint64_t candidate_count{};
        const bool valid_counts = axisCount(first->x, last->x, x_count) &&
            axisCount(first->y, last->y, y_count) && axisCount(first->z, last->z, z_count) &&
            checkedMultiply(x_count, y_count, plane_count) &&
            checkedMultiply(plane_count, z_count, candidate_count) &&
            candidate_count <= std::numeric_limits<std::size_t>::max();
        if (!valid_counts)
        {
            return lux::cxx::unexpected(failure(ESpatial3DPartitionIndexError::COORDINATE_OVERFLOW));
        }

        const std::size_t required_capacity = static_cast<std::size_t>(candidate_count);
        if (required_capacity > output.size())
        {
            return lux::cxx::unexpected(failure(
                ESpatial3DPartitionIndexError::OUTPUT_CAPACITY_EXCEEDED,
                *first,
                {},
                required_capacity
            ));
        }

        std::size_t written{};
        for (std::int64_t x = first->x;; ++x)
        {
            for (std::int64_t y = first->y;; ++y)
            {
                for (std::int64_t z = first->z;; ++z)
                {
                    if (const auto* value = find(math::GridCoord3i64{x, y, z}); value != nullptr)
                    {
                        output[written++] = *value;
                    }
                    if (z == last->z)
                    {
                        break;
                    }
                }
                if (y == last->y)
                {
                    break;
                }
            }
            if (x == last->x)
            {
                break;
            }
        }
        return written;
    }

    math::Position3d Spatial3DPartitionIndex::gridOrigin() const noexcept
    {
        return grid_origin_;
    }

    double Spatial3DPartitionIndex::cellWorldSize() const noexcept
    {
        return cell_world_size_;
    }

    std::span<const Spatial3DPartitionIndexEntry> Spatial3DPartitionIndex::entries() const noexcept
    {
        return entries_;
    }
}
