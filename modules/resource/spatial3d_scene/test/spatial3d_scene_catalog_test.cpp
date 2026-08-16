#include <lux/engine/resource/spatial3d_scene/Spatial3DSceneCatalog.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    [[nodiscard]] lux::entity_scene::EntitySectionId section(
        const char* value)
    {
        return lux::entity_scene::EntitySectionId{
            uuids::uuid::from_string(value).value()};
    }
}

int main()
{
    using namespace lux::spatial3d_scene;
    Spatial3DSceneCatalogConfig config;
    config.bands.push_back({
        Spatial3DSourceId{"lux.spatial3d.source.test"},
        lux::entity_scene::DemandChannelId{
            std::string{kSpatial3DResidentDemandChannelName}},
        0u,
        64.0,
        1.0,
        1.0});
    config.bands.push_back({
        Spatial3DSourceId{"lux.spatial3d.source.test"},
        lux::entity_scene::DemandChannelId{
            std::string{kSpatial3DVisualLodDemandChannelName}},
        1u,
        256.0,
        4.0,
        4.0});
    config.bands.push_back({
        Spatial3DSourceId{"lux.spatial3d.source.other"},
        lux::entity_scene::DemandChannelId{
            std::string{kSpatial3DResidentDemandChannelName}},
        0u,
        64.0,
        1.0,
        1.0});
    config.entries.push_back({
        {-2, 0, 3},
        0u,
        section("20000000-0000-4000-8000-000000000002")});
    config.entries.push_back({
        {-1, 0, 0},
        1u,
        section("20000000-0000-4000-8000-000000000001")});
    // Same coordinate and logical band shape is legal for another source.
    config.entries.push_back({
        {-2, 0, 3},
        2u,
        section("20000000-0000-4000-8000-000000000003")});
    const auto first = encodeSpatial3DSceneCatalog(config);
    std::ranges::reverse(config.bands);
    for (auto& entry : config.entries)
        entry.band = 2u - entry.band;
    std::ranges::reverse(config.entries);
    const auto second = encodeSpatial3DSceneCatalog(config);
    assert(first && second && *first == *second);
    const auto decoded = decodeSpatial3DSceneCatalog(*first);
    assert(decoded && decoded->bands.size() == 3u &&
           decoded->entries.size() == 3u);
    assert(decoded->residency == config.residency);
    assert((decoded->entries.front().coordinate ==
        lux::spatial::GridCoord3i64{-2, 0, 3}));

    // Runtime demand-source identity is (source, channel, level). Cell size
    // and distance scales are policy, so they cannot make a second band with
    // the same stable identity legal.
    auto duplicate_identity = *decoded;
    auto colliding_band = duplicate_identity.bands.front();
    colliding_band.cell_world_size *= 2.0;
    duplicate_identity.bands.push_back(std::move(colliding_band));
    duplicate_identity.entries.push_back({
        {9, 9, 9},
        static_cast<std::uint32_t>(duplicate_identity.bands.size() - 1u),
        section("20000000-0000-4000-8000-000000000004")});
    const auto duplicate_band = encodeSpatial3DSceneCatalog(
        std::move(duplicate_identity));
    assert(!duplicate_band && duplicate_band.error().error ==
        ESpatial3DSceneCatalogError::INVALID_BAND);

    auto nan_band = *decoded;
    nan_band.bands.front().cell_world_size =
        std::numeric_limits<double>::quiet_NaN();
    const auto invalid_nan = encodeSpatial3DSceneCatalog(
        std::move(nan_band));
    assert(!invalid_nan && invalid_nan.error().error ==
        ESpatial3DSceneCatalogError::INVALID_BAND);

    auto malformed = *first;
    malformed.pop_back();
    assert(!decodeSpatial3DSceneCatalog(malformed));

    // Count fields must be rejected from the remaining wire size before the
    // decoder reserves their declared container capacity.
    auto impossible_counts = std::vector<std::byte>(
        first->begin(), first->begin() + 40u);
    // Keep the declaration within the configured count limit so this case
    // specifically exercises the remaining-byte preflight rather than the
    // independent LIMIT_EXCEEDED gate.
    const std::uint32_t impossible_count = 4096u;
    std::memcpy(
        impossible_counts.data() + 8u,
        &impossible_count,
        sizeof(impossible_count));
    std::memcpy(
        impossible_counts.data() + 12u,
        &impossible_count,
        sizeof(impossible_count));
    const auto impossible = decodeSpatial3DSceneCatalog(impossible_counts);
    assert(!impossible &&
           impossible.error().error ==
               ESpatial3DSceneCatalogError::TRUNCATED);
    return 0;
}
