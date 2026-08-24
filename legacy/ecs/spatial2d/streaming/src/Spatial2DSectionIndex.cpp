#include <lux/engine/ecs/spatial2d/streaming/Spatial2DSectionIndex.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lux::ecs::spatial2d::streaming
{
    namespace
    {
        [[nodiscard]] bool coordinateLess(
            const Spatial2DSectionIndexEntry& lhs,
            const Spatial2DSectionIndexEntry& rhs) noexcept
        {
            return lhs.coordinate < rhs.coordinate;
        }

        [[nodiscard]] bool sectionLess(
            const Spatial2DSectionIndexEntry& lhs,
            const Spatial2DSectionIndexEntry& rhs) noexcept
        {
            return lhs.section.value() < rhs.section.value();
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

        [[nodiscard]] bool positionToCoordinate(
            double value,
            double section_world_size,
            std::int64_t& result) noexcept
        {
            const auto coordinate = std::floor(
                static_cast<long double>(value) /
                static_cast<long double>(section_world_size));
            constexpr long double kInt64Minimum =
                -9223372036854775808.0L;
            constexpr long double kInt64Limit =
                9223372036854775808.0L;
            if (coordinate < kInt64Minimum || coordinate >= kInt64Limit)
                return false;
            result = static_cast<std::int64_t>(coordinate);
            return true;
        }
    }

    lux::cxx::expected<
        lux::math::GridCoord2i64,
        Spatial2DIndexFailure>
    spatial2DSectionCoordinate(
        const lux::math::Position2d& position,
        double section_world_size) noexcept
    {
        if (!lux::math::isFinite(position) ||
            !std::isfinite(section_world_size) ||
            !(section_world_size > 0.0))
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::INVALID_POSITION});
        }
        lux::math::GridCoord2i64 result;
        if (!positionToCoordinate(
                position.x, section_world_size, result.x) ||
            !positionToCoordinate(
                position.y, section_world_size, result.y))
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::COORDINATE_OVERFLOW});
        }
        return result;
    }

    lux::cxx::expected<Spatial2DSectionIndex, Spatial2DIndexFailure>
    Spatial2DSectionIndex::create(
        std::vector<Spatial2DSectionIndexEntry> entries)
    {
        if (entries.empty())
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::EMPTY_INDEX});
        }
        for (const auto& entry : entries)
        {
            if (entry.section.empty())
            {
                return lux::cxx::unexpected(Spatial2DIndexFailure{
                    .code = ESpatial2DIndexError::INVALID_SECTION,
                    .coordinate = entry.coordinate,
                    .section = entry.section});
            }
        }
        std::sort(entries.begin(), entries.end(), coordinateLess);
        const auto duplicate_coordinate = std::adjacent_find(
            entries.begin(),
            entries.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.coordinate == rhs.coordinate;
            });
        if (duplicate_coordinate != entries.end())
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::DUPLICATE_COORDINATE,
                .coordinate = duplicate_coordinate->coordinate,
                .section = duplicate_coordinate->section});
        }

        auto by_section = entries;
        std::sort(by_section.begin(), by_section.end(), sectionLess);
        const auto duplicate_section = std::adjacent_find(
            by_section.begin(),
            by_section.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.section == rhs.section;
            });
        if (duplicate_section != by_section.end())
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::DUPLICATE_SECTION,
                .coordinate = duplicate_section->coordinate,
                .section = duplicate_section->section});
        }
        return Spatial2DSectionIndex{std::move(entries)};
    }

    const lux::ecs::scene_format::EntitySectionId* Spatial2DSectionIndex::find(
        lux::math::GridCoord2i64 coordinate) const noexcept
    {
        const Spatial2DSectionIndexEntry key{coordinate, {}};
        const auto found = std::lower_bound(
            entries_.begin(), entries_.end(), key, coordinateLess);
        return found != entries_.end() && found->coordinate == coordinate
            ? &found->section
            : nullptr;
    }

    lux::cxx::expected<Spatial2DWindow, Spatial2DIndexFailure>
    Spatial2DSectionIndex::window(
        const lux::math::Position2d& position,
        double section_world_size) const noexcept
    {
        Spatial2DWindow result;
        auto center = spatial2DSectionCoordinate(
            position, section_world_size);
        if (!center)
            return lux::cxx::unexpected(std::move(center.error()));
        result.center = *center;

        std::size_t write = 0u;
        for (std::int64_t y = -kSpatial2DResidentRadius;
             y <= kSpatial2DResidentRadius;
             ++y)
        {
            for (std::int64_t x = -kSpatial2DResidentRadius;
                 x <= kSpatial2DResidentRadius;
                 ++x)
            {
                lux::math::GridCoord2i64 coordinate;
                if (!checkedOffset(result.center.x, x, coordinate.x) ||
                    !checkedOffset(result.center.y, y, coordinate.y))
                {
                    return lux::cxx::unexpected(Spatial2DIndexFailure{
                        .code = ESpatial2DIndexError::COORDINATE_OVERFLOW,
                        .coordinate = result.center});
                }
                const auto* section = find(coordinate);
                if (!section)
                {
                    return lux::cxx::unexpected(Spatial2DIndexFailure{
                        .code = ESpatial2DIndexError::SECTION_NOT_FOUND,
                        .coordinate = coordinate});
                }
                result.entries[write++] = Spatial2DWindowEntry{
                    coordinate,
                    *section,
                    std::nullopt,
                    std::abs(x) <= kSpatial2DActiveRadius &&
                        std::abs(y) <= kSpatial2DActiveRadius};
            }
        }
        return result;
    }

    Spatial2DSectionSource Spatial2DSectionSource::finite(Spatial2DSectionIndex index)
    {
        return Spatial2DSectionSource{
            std::optional<Spatial2DSectionIndex>{std::move(index)}, {}};
    }

    lux::cxx::expected<Spatial2DSectionSource, Spatial2DIndexFailure>
    Spatial2DSectionSource::procedural(Spatial2DSectionRecordFactory factory)
    {
        if (!factory)
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::EMPTY_INDEX});
        }
        return Spatial2DSectionSource{
            std::nullopt, std::move(factory)};
    }

    lux::cxx::expected<Spatial2DWindow, Spatial2DIndexFailure>
    Spatial2DSectionSource::window(
        const lux::math::Position2d& position,
        double section_world_size)
    {
        if (finite_)
            return finite_->window(position, section_world_size);
        if (!factory_)
        {
            return lux::cxx::unexpected(Spatial2DIndexFailure{
                .code = ESpatial2DIndexError::INVALID_POSITION});
        }

        Spatial2DWindow result;
        auto center = spatial2DSectionCoordinate(
            position, section_world_size);
        if (!center)
            return lux::cxx::unexpected(std::move(center.error()));
        result.center = *center;
        std::size_t write = 0u;
        for (std::int64_t y = -kSpatial2DResidentRadius;
             y <= kSpatial2DResidentRadius;
             ++y)
        {
            for (std::int64_t x = -kSpatial2DResidentRadius;
                 x <= kSpatial2DResidentRadius;
                 ++x)
            {
                lux::math::GridCoord2i64 coordinate;
                if (!checkedOffset(result.center.x, x, coordinate.x) ||
                    !checkedOffset(result.center.y, y, coordinate.y))
                {
                    return lux::cxx::unexpected(Spatial2DIndexFailure{
                        .code = ESpatial2DIndexError::COORDINATE_OVERFLOW,
                        .coordinate = result.center});
                }
                auto record = factory_(coordinate);
                if (!record)
                {
                    auto failure = std::move(record.error());
                    failure.coordinate = coordinate;
                    return lux::cxx::unexpected(std::move(failure));
                }
                if (record->id.empty())
                {
                    return lux::cxx::unexpected(Spatial2DIndexFailure{
                        .code = ESpatial2DIndexError::INVALID_SECTION,
                        .coordinate = coordinate});
                }
                const auto section = record->id;
                result.entries[write++] = Spatial2DWindowEntry{
                    coordinate,
                    section,
                    std::optional<lux::ecs::scene_format::SectionRecord>{
                        std::move(*record)},
                    std::abs(x) <= kSpatial2DActiveRadius &&
                        std::abs(y) <= kSpatial2DActiveRadius};
            }
        }
        return result;
    }
}
