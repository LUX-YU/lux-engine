#include <lux/engine/spatial/Spatial2DPartitionIndex.hpp>

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
            const Spatial2DPartitionIndexEntry& left,
            const Spatial2DPartitionIndexEntry& right
        ) noexcept
        {
            return left.coordinate < right.coordinate;
        }

        [[nodiscard]] Spatial2DPartitionIndexFailure failure(
            ESpatial2DPartitionIndexError code,
            math::GridCoord2i64 coordinate = {},
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
    }

    Spatial2DPartitionIndex::Spatial2DPartitionIndex(
        math::Position2d grid_origin,
        double cell_world_size,
        std::vector<Spatial2DPartitionIndexEntry> entries
    ) noexcept
        : grid_origin_(grid_origin), cell_world_size_(cell_world_size), entries_(std::move(entries))
    {
    }

    lux::cxx::expected<Spatial2DPartitionIndex, Spatial2DPartitionIndexFailure>
    Spatial2DPartitionIndex::create(
        math::Position2d grid_origin,
        double cell_world_size,
        std::vector<Spatial2DPartitionIndexEntry> entries
    ) noexcept
    {
        if (!math::isFinite(grid_origin) || !std::isfinite(cell_world_size) || !(cell_world_size > 0.0))
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::INVALID_GRID));
        }
        if (entries.empty())
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::EMPTY_INDEX));
        }

        try
        {
            for (const auto& entry : entries)
            {
                if (entry.partition.value == std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(failure(
                        ESpatial2DPartitionIndexError::INVALID_PARTITION,
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
                    ESpatial2DPartitionIndexError::DUPLICATE_COORDINATE,
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
                    ESpatial2DPartitionIndexError::DUPLICATE_PARTITION,
                    duplicate_partition->coordinate,
                    duplicate_partition->partition
                ));
            }
            return Spatial2DPartitionIndex(grid_origin, cell_world_size, std::move(entries));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<math::GridCoord2i64, Spatial2DPartitionIndexFailure>
    Spatial2DPartitionIndex::coordinate(math::Position2d position) const noexcept
    {
        if (!math::isFinite(position))
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::INVALID_POSITION));
        }
        math::GridCoord2i64 result;
        if (!coordinateValue(position.x, grid_origin_.x, cell_world_size_, result.x) ||
            !coordinateValue(position.y, grid_origin_.y, cell_world_size_, result.y))
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::COORDINATE_OVERFLOW));
        }
        return result;
    }

    const partition::PartitionOrdinal* Spatial2DPartitionIndex::find(math::GridCoord2i64 coordinate) const noexcept
    {
        const Spatial2DPartitionIndexEntry key{coordinate, {}};
        const auto found = std::lower_bound(entries_.begin(), entries_.end(), key, coordinateLess);
        return found != entries_.end() && found->coordinate == coordinate ? &found->partition : nullptr;
    }

    lux::cxx::expected<std::optional<partition::PartitionOrdinal>, Spatial2DPartitionIndexFailure>
    Spatial2DPartitionIndex::find(math::Position2d position) const noexcept
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

    lux::cxx::expected<std::size_t, Spatial2DPartitionIndexFailure> Spatial2DPartitionIndex::query(
        const Spatial2DQueryBounds& bounds,
        std::span<partition::PartitionOrdinal> output
    ) const noexcept
    {
        const bool is_finite = math::isFinite(bounds.minimum) && math::isFinite(bounds.maximum);
        const bool is_ordered = bounds.minimum.x < bounds.maximum.x && bounds.minimum.y < bounds.maximum.y;
        if (!is_finite || !is_ordered)
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::INVALID_BOUNDS));
        }

        auto first = coordinate(bounds.minimum);
        const math::Position2d inclusive_maximum{
            std::nextafter(bounds.maximum.x, -std::numeric_limits<double>::infinity()),
            std::nextafter(bounds.maximum.y, -std::numeric_limits<double>::infinity())
        };
        auto last = coordinate(inclusive_maximum);
        if (!first || !last)
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::COORDINATE_OVERFLOW));
        }

        std::uint64_t x_count{};
        std::uint64_t y_count{};
        const bool valid_counts = axisCount(first->x, last->x, x_count) &&
            axisCount(first->y, last->y, y_count) &&
            (y_count == 0U || x_count <= std::numeric_limits<std::uint64_t>::max() / y_count);
        if (!valid_counts)
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::COORDINATE_OVERFLOW));
        }
        const std::uint64_t candidate_count = x_count * y_count;
        if (candidate_count > std::numeric_limits<std::size_t>::max())
        {
            return lux::cxx::unexpected(failure(ESpatial2DPartitionIndexError::COORDINATE_OVERFLOW));
        }

        const std::size_t required_capacity = static_cast<std::size_t>(candidate_count);
        if (required_capacity > output.size())
        {
            return lux::cxx::unexpected(failure(
                ESpatial2DPartitionIndexError::OUTPUT_CAPACITY_EXCEEDED,
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
                if (const auto* value = find(math::GridCoord2i64{x, y}); value != nullptr)
                {
                    output[written++] = *value;
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

    math::Position2d Spatial2DPartitionIndex::gridOrigin() const noexcept
    {
        return grid_origin_;
    }

    double Spatial2DPartitionIndex::cellWorldSize() const noexcept
    {
        return cell_world_size_;
    }

    std::span<const Spatial2DPartitionIndexEntry> Spatial2DPartitionIndex::entries() const noexcept
    {
        return entries_;
    }
}
