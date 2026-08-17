#include <lux/engine/runtime/spatial2d/infinite/Spatial2DSectionIndex.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    lux::entity_scene::EntitySectionId sectionId(std::uint64_t ordinal)
    {
        std::array<std::uint8_t, 16u> bytes{};
        bytes[6] = 0x40u;
        bytes[8] = 0x80u;
        for (std::size_t index = 0u; index < 8u; ++index)
        {
            bytes[15u - index] = static_cast<std::uint8_t>(
                ordinal >> (index * 8u));
        }
        return lux::entity_scene::EntitySectionId{uuids::uuid{bytes}};
    }

    lux::entity_scene::EntitySectionId coordinateSectionId(
        lux::spatial::GridCoord2i64 coordinate)
    {
        auto mix = [](std::uint64_t value) noexcept
        {
            value ^= value >> 30u;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27u;
            value *= 0x94d049bb133111ebull;
            return value ^ (value >> 31u);
        };
        return sectionId(
            mix(static_cast<std::uint64_t>(coordinate.x)) ^
            (mix(static_cast<std::uint64_t>(coordinate.y)) << 1u));
    }

    void addWindow(
        std::vector<lux::runtime::spatial2d::Spatial2DSectionIndexEntry>& entries,
        lux::spatial::GridCoord2i64 center,
        std::uint64_t& ordinal)
    {
        for (std::int64_t y = -2; y <= 2; ++y)
        {
            for (std::int64_t x = -2; x <= 2; ++x)
            {
                entries.push_back({
                    {center.x + x, center.y + y}, sectionId(ordinal++)});
            }
        }
    }
}

int main()
{
    namespace spatial2d = lux::runtime::spatial2d;
    std::vector<spatial2d::Spatial2DSectionIndexEntry> entries;
    std::uint64_t ordinal = 1u;
    addWindow(entries, {0, 0}, ordinal);
    addWindow(entries, {-10, -10}, ordinal);
    addWindow(entries, {39, 39}, ordinal); // floor(10k / 256)
    addWindow(entries, {1'000'000, -1'000'000}, ordinal);

    auto index = spatial2d::Spatial2DSectionIndex::create(
        std::move(entries));
    assert(index);

    const auto origin = index->window({0.0, 0.0}, 64.0);
    assert(origin);
    assert((origin->center == lux::spatial::GridCoord2i64{0, 0}));
    assert(std::count_if(
        origin->entries.begin(), origin->entries.end(),
        [](const auto& entry) { return entry.active; }) ==
        spatial2d::kSpatial2DActiveSectionCount);
    assert(origin->entries.size() ==
        spatial2d::kSpatial2DResidentSectionCount);

    // Floor division, rather than truncation, owns the negative boundary.
    const auto negative = index->window({-640.0, -640.0}, 64.0);
    assert(negative);
    assert((negative->center == lux::spatial::GridCoord2i64{-10, -10}));

    // 10k cells at 0.25 units/cell lies in chunk floor(10000 / 256).
    const auto ten_k_cells = index->window({2500.0, 2500.0}, 64.0);
    assert(ten_k_cells);
    assert((ten_k_cells->center == lux::spatial::GridCoord2i64{39, 39}));

    constexpr double kFar = 64.0 * 1'000'000.0;
    const auto far_window = index->window({kFar, -kFar}, 64.0);
    assert(far_window);
    assert((far_window->center == lux::spatial::GridCoord2i64{1'000'000, -1'000'000}));

    auto procedural = spatial2d::Spatial2DSectionSource::procedural(
        [](lux::spatial::GridCoord2i64 coordinate)
            -> lux::cxx::expected<
                lux::entity_scene::EntitySectionRecord,
                spatial2d::Spatial2DIndexFailure>
        {
            lux::entity_scene::EntitySectionRecord record;
            record.id = coordinateSectionId(coordinate);
            return record;
        });
    assert(procedural);
    const auto arbitrary = procedural->window({64.0 * 7'654'321.0, 64.0 * -6'543'210.0}, 64.0);
    assert(arbitrary);
    assert((arbitrary->center == lux::spatial::GridCoord2i64{7'654'321, -6'543'210}));
    assert(std::ranges::all_of(
        arbitrary->entries,
        [](const auto& entry) 
        { 
            return entry.record.has_value(); 
        })
    );

    const auto non_finite = index->window({std::numeric_limits<double>::infinity(), 0.0}, 64.0);
    assert(!non_finite);
    assert(non_finite.error().code == spatial2d::ESpatial2DIndexError::INVALID_POSITION);

    auto duplicate_entries = std::vector<
        spatial2d::Spatial2DSectionIndexEntry>{
            {{0, 0}, sectionId(500u)},
            {{0, 0}, sectionId(501u)}
        };
    const auto duplicate = spatial2d::Spatial2DSectionIndex::create(
        std::move(duplicate_entries)
    );
    assert(!duplicate);
    assert(duplicate.error().code ==
        spatial2d::ESpatial2DIndexError::DUPLICATE_COORDINATE);
    return 0;
}
