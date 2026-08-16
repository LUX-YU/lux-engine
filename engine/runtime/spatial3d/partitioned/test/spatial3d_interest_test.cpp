#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DSectionSource.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
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

    std::uint64_t mix(std::uint64_t value) noexcept
    {
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebull;
        return value ^ (value >> 31u);
    }

    lux::entity_scene::EntitySectionId coordinateSectionId(
        lux::spatial::GridCoord3i64 coordinate)
    {
        return sectionId(
            mix(static_cast<std::uint64_t>(coordinate.x)) ^
            (mix(static_cast<std::uint64_t>(coordinate.y)) << 1u) ^
            (mix(static_cast<std::uint64_t>(coordinate.z)) << 2u));
    }

    lux::entity_scene::EntitySectionRecord record(
        lux::spatial::GridCoord3i64 coordinate,
        std::uint64_t ordinal)
    {
        lux::entity_scene::EntitySectionRecord result;
        result.id = ordinal == 0u
            ? coordinateSectionId(coordinate)
            : sectionId(ordinal);
        result.source = lux::entity_scene::StoredSectionSource{
            "/Game/Sections/Spatial3D_lxes"};
        result.content_digest[0] = std::byte{1u};
        result.encoded_bytes = 1u;
        result.decoded_bytes = 1u;
        result.entity_count = 1u;
        result.demand_channels.emplace_back("org.lux.test.spatial3d");
        return result;
    }
}

int main()
{
    namespace spatial3d = lux::runtime::spatial3d;

    const auto negative = spatial3d::spatial3DSectionCoordinate(
        {-0.001, -64.001, 63.999}, 64.0);
    assert(negative);
    assert((*negative == lux::spatial::GridCoord3i64{-1, -2, 0}));

    constexpr double kLarge = 1'000'000'000'000.0;
    const auto large = spatial3d::spatial3DSectionCoordinate(
        {kLarge, -kLarge, kLarge + 0.001}, 64.0);
    assert(large);
    assert((*large == lux::spatial::GridCoord3i64{
        15'625'000'000ll, -15'625'000'000ll, 15'625'000'000ll}));

    const auto non_finite = spatial3d::spatial3DSectionCoordinate(
        {0.0, std::numeric_limits<double>::infinity(), 0.0}, 64.0);
    assert(!non_finite);
    assert(non_finite.error().code ==
        spatial3d::ESpatial3DSourceError::INVALID_REQUEST);

    auto rules = spatial3d::Spatial3DSectionSource::ruleGrid(
        [](lux::spatial::GridCoord3i64 coordinate)
            -> lux::cxx::expected<
                lux::entity_scene::EntitySectionRecord,
                spatial3d::Spatial3DSourceFailure>
        {
            return record(coordinate, 0u);
        });
    assert(rules);
    const spatial3d::Spatial3DWindowRequest origin_request{
        .center = {0.0, 0.0, 0.0},
        .cell_world_size = 64.0,
        .active_distance = 64.0,
        .resident_distance = 128.0,
        .maximum_sections = 256u};
    const auto origin = rules->window(origin_request);
    assert(origin);
    assert((origin->center == lux::spatial::GridCoord3i64{0, 0, 0}));
    assert(origin->active_sections == 27u);
    assert(origin->entries.size() == 125u);
    assert(origin->records.size() == origin->entries.size());

    auto predicted_request = origin_request;
    predicted_request.prediction_offset_x = 256.0;
    const auto predicted = rules->window(predicted_request);
    assert(predicted);
    assert((predicted->predicted_center ==
        lux::spatial::GridCoord3i64{4, 0, 0}));
    assert(predicted->active_sections == 27u);
    assert(predicted->entries.size() == 225u);

    predicted_request.maximum_sections = 224u;
    const auto bounded = rules->window(predicted_request);
    assert(!bounded);
    assert(bounded.error().code ==
        spatial3d::ESpatial3DSourceError::WINDOW_LIMIT_EXCEEDED);
    assert(bounded.error().requested_sections == 225u);

    std::vector<spatial3d::Spatial3DSectionCatalogEntry> entries;
    std::uint64_t ordinal = 1u;
    for (std::int64_t x = -2; x <= 2; ++x)
    {
        for (std::int64_t y = -2; y <= 2; ++y)
        {
            for (std::int64_t z = -2; z <= 2; ++z)
            {
                const lux::spatial::GridCoord3i64 coordinate{x, y, z};
                entries.push_back({
                    coordinate, record(coordinate, ordinal++).id});
            }
        }
    }
    auto catalog = spatial3d::Spatial3DSectionCatalog::create(
        std::move(entries));
    assert(catalog);
    assert(catalog->entries().size() == 125u);
    auto finite = spatial3d::Spatial3DSectionSource::catalog(
        std::move(*catalog));
    const auto finite_origin = finite.window(origin_request);
    assert(finite_origin);
    assert(finite_origin->entries.size() == 125u);
    assert(finite_origin->records.empty());

    auto missing_request = origin_request;
    missing_request.center = {64.0, 0.0, 0.0};
    const auto missing = finite.window(missing_request);
    assert(missing);
    assert(missing->entries.size() < finite_origin->entries.size());

    std::vector<spatial3d::Spatial3DSectionCatalogEntry> duplicate{
        {{0, 0, 0}, record({0, 0, 0}, 500u).id},
        {{0, 0, 0}, record({0, 0, 0}, 501u).id}};
    const auto rejected = spatial3d::Spatial3DSectionCatalog::create(
        std::move(duplicate));
    assert(!rejected);
    assert(rejected.error().code ==
        spatial3d::ESpatial3DSourceError::DUPLICATE_COORDINATE);
    return 0;
}
