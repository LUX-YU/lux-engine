#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DSectionSource.hpp>

#include <lux/engine/scene/ScenePackageCodec.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace lux::runtime::spatial3d
{
    namespace
    {
        [[nodiscard]] bool coordinateLess(
            const Spatial3DSectionCatalogEntry& lhs,
            const Spatial3DSectionCatalogEntry& rhs) noexcept
        {
            return lhs.coordinate < rhs.coordinate;
        }

        [[nodiscard]] bool checkedOffset(
            std::int64_t value,
            std::int64_t offset,
            std::int64_t& result) noexcept
        {
            if (offset > 0 &&
                value > std::numeric_limits<std::int64_t>::max() - offset)
            {
                return false;
            }
            if (offset < 0 &&
                value < std::numeric_limits<std::int64_t>::min() - offset)
            {
                return false;
            }
            result = value + offset;
            return true;
        }

        [[nodiscard]] bool positionAxisToCoordinate(
            double value,
            double cell_world_size,
            std::int64_t& result) noexcept
        {
            const auto coordinate = std::floor(
                static_cast<long double>(value) /
                static_cast<long double>(cell_world_size));
            constexpr long double kInt64Minimum =
                -9223372036854775808.0L;
            constexpr long double kInt64Limit =
                9223372036854775808.0L;
            if (coordinate < kInt64Minimum || coordinate >= kInt64Limit)
                return false;
            result = static_cast<std::int64_t>(coordinate);
            return true;
        }

        [[nodiscard]] bool radiusCells(
            double distance,
            double cell_world_size,
            std::int64_t& result) noexcept
        {
            if (!std::isfinite(distance) || distance < 0.0 ||
                !std::isfinite(cell_world_size) || cell_world_size <= 0.0)
            {
                return false;
            }
            const auto cells = std::ceil(
                static_cast<long double>(distance) /
                static_cast<long double>(cell_world_size));
            constexpr long double kInt64Limit =
                9223372036854775808.0L;
            if (cells < 0.0L || cells >= kInt64Limit)
                return false;
            result = static_cast<std::int64_t>(cells);
            return true;
        }

        [[nodiscard]] bool cubeCount(
            std::int64_t radius,
            std::size_t& result) noexcept
        {
            if (radius < 0)
                return false;
            const auto unsigned_radius = static_cast<std::uint64_t>(radius);
            if (unsigned_radius >
                (std::numeric_limits<std::size_t>::max() - 1u) / 2u)
            {
                return false;
            }
            const auto side = static_cast<std::size_t>(
                unsigned_radius * 2u + 1u);
            if (side != 0u && side >
                std::numeric_limits<std::size_t>::max() / side)
            {
                return false;
            }
            const auto square = side * side;
            if (side != 0u && square >
                std::numeric_limits<std::size_t>::max() / side)
            {
                return false;
            }
            result = square * side;
            return true;
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<lux::math::GridCoord3i64>,
            Spatial3DSourceFailure>
        coordinates(
            lux::math::GridCoord3i64 center,
            std::int64_t radius,
            std::size_t maximum_sections)
        {
            std::size_t count = 0u;
            if (!cubeCount(radius, count) || count > maximum_sections)
            {
                return lux::cxx::unexpected(Spatial3DSourceFailure{
                    .code = ESpatial3DSourceError::WINDOW_LIMIT_EXCEEDED,
                    .coordinate = center,
                    .requested_sections = count,
                    .maximum_sections = maximum_sections});
            }
            std::vector<lux::math::GridCoord3i64> result;
            result.reserve(count);
            for (std::int64_t x = -radius; x <= radius; ++x)
            {
                for (std::int64_t y = -radius; y <= radius; ++y)
                {
                    for (std::int64_t z = -radius; z <= radius; ++z)
                    {
                        lux::math::GridCoord3i64 coordinate;
                        if (!checkedOffset(center.x, x, coordinate.x) ||
                            !checkedOffset(center.y, y, coordinate.y) ||
                            !checkedOffset(center.z, z, coordinate.z))
                        {
                            return lux::cxx::unexpected(
                                Spatial3DSourceFailure{
                                    .code = ESpatial3DSourceError::
                                        COORDINATE_OVERFLOW,
                                    .coordinate = center});
                        }
                        result.push_back(coordinate);
                    }
                }
            }
            return result;
        }

        [[nodiscard]] std::uint64_t axisDistance(
            std::int64_t lhs,
            std::int64_t rhs) noexcept
        {
            constexpr std::uint64_t kSign = std::uint64_t{1u} << 63u;
            const auto ordered_lhs = static_cast<std::uint64_t>(lhs) ^ kSign;
            const auto ordered_rhs = static_cast<std::uint64_t>(rhs) ^ kSign;
            return ordered_lhs >= ordered_rhs
                ? ordered_lhs - ordered_rhs
                : ordered_rhs - ordered_lhs;
        }

        [[nodiscard]] bool insideCube(
            lux::math::GridCoord3i64 coordinate,
            lux::math::GridCoord3i64 center,
            std::int64_t radius) noexcept
        {
            const auto extent = static_cast<std::uint64_t>(radius);
            return axisDistance(coordinate.x, center.x) <= extent &&
                axisDistance(coordinate.y, center.y) <= extent &&
                axisDistance(coordinate.z, center.z) <= extent;
        }

        [[nodiscard]] bool sectionLess(
            const Spatial3DWindowEntry* lhs,
            const Spatial3DWindowEntry* rhs) noexcept
        {
            return lhs->section.value() < rhs->section.value();
        }
    }

    lux::cxx::expected<
        lux::math::GridCoord3i64,
        Spatial3DSourceFailure>
    spatial3DSectionCoordinate(
        const lux::math::Position3d& position,
        double cell_world_size) noexcept
    {
        if (!lux::math::isFinite(position) ||
            !std::isfinite(cell_world_size) || cell_world_size <= 0.0)
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::INVALID_REQUEST});
        }
        lux::math::GridCoord3i64 result;
        if (!positionAxisToCoordinate(
                position.x, cell_world_size, result.x) ||
            !positionAxisToCoordinate(
                position.y, cell_world_size, result.y) ||
            !positionAxisToCoordinate(
                position.z, cell_world_size, result.z))
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::COORDINATE_OVERFLOW});
        }
        return result;
    }

    lux::cxx::expected<Spatial3DSectionCatalog, Spatial3DSourceFailure>
    Spatial3DSectionCatalog::create(
        std::vector<Spatial3DSectionCatalogEntry> entries)
    {
        if (entries.empty())
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::EMPTY_CATALOG});
        }
        for (const auto& entry : entries)
        {
            if (entry.section.empty())
            {
                return lux::cxx::unexpected(Spatial3DSourceFailure{
                    .code = ESpatial3DSourceError::INVALID_RECORD,
                    .coordinate = entry.coordinate,
                    .section = entry.section});
            }
        }
        std::sort(entries.begin(), entries.end(), coordinateLess);
        const auto duplicate_coordinate = std::adjacent_find(
            entries.begin(), entries.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.coordinate == rhs.coordinate;
            });
        if (duplicate_coordinate != entries.end())
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::DUPLICATE_COORDINATE,
                .coordinate = duplicate_coordinate->coordinate,
                .section = duplicate_coordinate->section});
        }
        std::vector<const Spatial3DSectionCatalogEntry*> by_section;
        by_section.reserve(entries.size());
        for (const auto& entry : entries)
            by_section.push_back(&entry);
        std::sort(
            by_section.begin(), by_section.end(),
            [](const auto* lhs, const auto* rhs)
            {
                return lhs->section.value() < rhs->section.value();
            });
        const auto duplicate_section = std::adjacent_find(
            by_section.begin(), by_section.end(),
            [](const auto* lhs, const auto* rhs)
            {
                return lhs->section == rhs->section;
            });
        if (duplicate_section != by_section.end())
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::DUPLICATE_SECTION,
                .coordinate = (*duplicate_section)->coordinate,
                .section = (*duplicate_section)->section});
        }
        return Spatial3DSectionCatalog{std::move(entries)};
    }

    const Spatial3DSectionCatalogEntry* Spatial3DSectionCatalog::find(
        lux::math::GridCoord3i64 coordinate) const noexcept
    {
        const Spatial3DSectionCatalogEntry key{coordinate, {}};
        const auto found = std::lower_bound(
            entries_.begin(), entries_.end(), key, coordinateLess);
        return found != entries_.end() && found->coordinate == coordinate
            ? &*found
            : nullptr;
    }

    Spatial3DSectionSource Spatial3DSectionSource::catalog(Spatial3DSectionCatalog catalog)
    {
        return Spatial3DSectionSource{
            std::optional<Spatial3DSectionCatalog>{std::move(catalog)}, {}};
    }

    lux::cxx::expected<Spatial3DSectionSource, Spatial3DSourceFailure>
    Spatial3DSectionSource::ruleGrid(Spatial3DSectionRecordRule rule)
    {
        if (!rule)
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::RULE_FAILURE});
        }
        return Spatial3DSectionSource{std::nullopt, std::move(rule)};
    }

    lux::cxx::expected<Spatial3DWindow, Spatial3DSourceFailure>
    Spatial3DSectionSource::window(const Spatial3DWindowRequest& request)
    {
        if (request.maximum_sections == 0u ||
            !std::isfinite(request.active_distance) ||
            !std::isfinite(request.resident_distance) ||
            request.active_distance < 0.0 ||
            request.resident_distance < request.active_distance ||
            !std::isfinite(request.prediction_offset_x) ||
            !std::isfinite(request.prediction_offset_y) ||
            !std::isfinite(request.prediction_offset_z))
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::INVALID_REQUEST});
        }
        auto center = spatial3DSectionCoordinate(
            request.center, request.cell_world_size);
        if (!center)
            return lux::cxx::unexpected(std::move(center.error()));
        const lux::math::Position3d predicted_position{
            request.center.x + request.prediction_offset_x,
            request.center.y + request.prediction_offset_y,
            request.center.z + request.prediction_offset_z};
        auto predicted_center = spatial3DSectionCoordinate(
            predicted_position, request.cell_world_size);
        if (!predicted_center)
            return lux::cxx::unexpected(std::move(predicted_center.error()));

        std::int64_t active_radius = 0;
        std::int64_t resident_radius = 0;
        if (!radiusCells(
                request.active_distance,
                request.cell_world_size,
                active_radius) ||
            !radiusCells(
                request.resident_distance,
                request.cell_world_size,
                resident_radius))
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::INVALID_REQUEST});
        }

        auto current = coordinates(
            *center, resident_radius, request.maximum_sections);
        if (!current)
            return lux::cxx::unexpected(std::move(current.error()));
        auto predicted = coordinates(
            *predicted_center,
            resident_radius,
            request.maximum_sections);
        if (!predicted)
            return lux::cxx::unexpected(std::move(predicted.error()));

        std::vector<lux::math::GridCoord3i64> merged;
        const auto combined_capacity = current->size() >
                request.maximum_sections - predicted->size()
                ? request.maximum_sections
                : current->size() + predicted->size();
        merged.reserve(std::min(
            request.maximum_sections, combined_capacity));
        std::set_union(
            current->begin(), current->end(),
            predicted->begin(), predicted->end(),
            std::back_inserter(merged));
        if (merged.size() > request.maximum_sections)
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::WINDOW_LIMIT_EXCEEDED,
                .coordinate = *center,
                .requested_sections = merged.size(),
                .maximum_sections = request.maximum_sections});
        }

        Spatial3DWindow result;
        result.center = *center;
        result.predicted_center = *predicted_center;
        result.entries.reserve(merged.size());
        if (!catalog_)
            result.records.reserve(merged.size());
        for (const auto coordinate : merged)
        {
            const auto active = insideCube(
                coordinate, *center, active_radius);
            if (catalog_)
            {
                const auto* entry = catalog_->find(coordinate);
                if (!entry)
                    continue;
                result.entries.push_back({
                    coordinate, entry->section, active});
            }
            else
            {
                if (!rule_)
                {
                    return lux::cxx::unexpected(Spatial3DSourceFailure{
                        .code = ESpatial3DSourceError::RULE_FAILURE,
                        .coordinate = coordinate});
                }
                auto record = rule_(coordinate);
                if (!record)
                {
                    auto failure = std::move(record.error());
                    failure.coordinate = coordinate;
                    return lux::cxx::unexpected(std::move(failure));
                }
                if (!lux::scene::validateSectionRecord(*record))
                {
                    return lux::cxx::unexpected(Spatial3DSourceFailure{
                        .code = ESpatial3DSourceError::INVALID_RECORD,
                        .coordinate = coordinate,
                        .section = record->id});
                }
                const auto section = record->id;
                result.entries.push_back({
                    coordinate, section, active});
                result.records.push_back(std::move(*record));
            }
            result.active_sections += active ? 1u : 0u;
        }

        std::vector<const Spatial3DWindowEntry*> by_section;
        by_section.reserve(result.entries.size());
        for (const auto& entry : result.entries)
            by_section.push_back(&entry);
        std::sort(by_section.begin(), by_section.end(), sectionLess);
        const auto duplicate = std::adjacent_find(
            by_section.begin(), by_section.end(),
            [](const auto* lhs, const auto* rhs)
            {
                return lhs->section == rhs->section;
            });
        if (duplicate != by_section.end())
        {
            return lux::cxx::unexpected(Spatial3DSourceFailure{
                .code = ESpatial3DSourceError::DUPLICATE_SECTION,
                .coordinate = (*duplicate)->coordinate,
                .section = (*duplicate)->section});
        }
        return result;
    }
}
