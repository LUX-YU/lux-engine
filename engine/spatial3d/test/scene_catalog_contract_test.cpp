#include <lux/engine/spatial3d/SceneCatalog.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    // Frozen L3SC v1 fixture produced before the legacy Resource component was
    // removed. Keeping the bytes in the canonical owner's contract test makes
    // wire compatibility independent of a second codec implementation.
    inline constexpr std::array<std::byte, 164u> kL3scV1Golden{
        std::byte{0x4c}, std::byte{0x33}, std::byte{0x53}, std::byte{0x43}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x19}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x6c}, std::byte{0x75}, std::byte{0x78}, std::byte{0x2e},
        std::byte{0x73}, std::byte{0x70}, std::byte{0x61}, std::byte{0x74}, std::byte{0x69}, std::byte{0x61}, std::byte{0x6c}, std::byte{0x33}, std::byte{0x64}, std::byte{0x2e}, std::byte{0x73}, std::byte{0x6f},
        std::byte{0x75}, std::byte{0x72}, std::byte{0x63}, std::byte{0x65}, std::byte{0x2e}, std::byte{0x74}, std::byte{0x65}, std::byte{0x73}, std::byte{0x74}, std::byte{0x16}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x6c}, std::byte{0x75}, std::byte{0x78}, std::byte{0x2e}, std::byte{0x73}, std::byte{0x70}, std::byte{0x61}, std::byte{0x74}, std::byte{0x69}, std::byte{0x61}, std::byte{0x6c},
        std::byte{0x33}, std::byte{0x64}, std::byte{0x2e}, std::byte{0x72}, std::byte{0x65}, std::byte{0x73}, std::byte{0x69}, std::byte{0x64}, std::byte{0x65}, std::byte{0x6e}, std::byte{0x74}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x50}, std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xf0}, std::byte{0x3f}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xf8}, std::byte{0x3f},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00},
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
    };
}

int main()
{
    namespace canonical = lux::spatial3d;
    static_assert(std::is_same_v<
        decltype(canonical::SceneCatalogEntry::section),
        lux::ecs::scene_format::EntitySectionId>);
    static_assert(std::is_same_v<
        decltype(canonical::SceneCatalogBand::demand_channel),
        lux::scene::DemandChannelId>);

    canonical::SceneCatalog catalog;
    catalog.residency = {4096u, 128u, 4u, 16u};
    catalog.bands.push_back({
        canonical::SourceId{"lux.spatial3d.source.test"},
        lux::scene::DemandChannelId{canonical::kResidentDemandChannelName},
        0u,
        64.0,
        1.0,
        1.5});
    catalog.entries.push_back({
        {1, 2, 3},
        0u,
        lux::ecs::scene_format::EntitySectionId{
            uuid("aaaaaaaa-0000-4000-8000-000000000001")}});

    const auto encoded = canonical::encodeSceneCatalog(catalog);
    assert(encoded);
    assert(std::ranges::equal(*encoded, kL3scV1Golden));

    const auto decoded = canonical::decodeSceneCatalog(kL3scV1Golden);
    assert(decoded);
    assert(*decoded == catalog);

    auto unordered = catalog;
    unordered.bands.push_back({
        canonical::SourceId{"lux.spatial3d.source.other"},
        lux::scene::DemandChannelId{canonical::kVisualLodDemandChannelName},
        1u,
        256.0,
        4.0,
        4.0});
    unordered.entries.push_back({
        {-1, 0, 0},
        1u,
        lux::ecs::scene_format::EntitySectionId{
            uuid("aaaaaaaa-0000-4000-8000-000000000002")}});
    std::ranges::reverse(unordered.bands);
    for (auto& entry : unordered.entries)
        entry.band = 1u - entry.band;
    const auto canonicalized = canonical::encodeSceneCatalog(unordered);
    assert(canonicalized);

    auto duplicate_band = catalog;
    duplicate_band.bands.push_back(duplicate_band.bands.front());
    duplicate_band.entries.push_back({
        {9, 9, 9},
        1u,
        lux::ecs::scene_format::EntitySectionId{
            uuid("aaaaaaaa-0000-4000-8000-000000000003")}});
    const auto duplicate_band_result =
        canonical::encodeSceneCatalog(std::move(duplicate_band));
    assert(!duplicate_band_result &&
        duplicate_band_result.error().error ==
            canonical::SceneCatalogError::InvalidBand);

    auto duplicate_entry = catalog;
    duplicate_entry.entries.push_back({
        {1, 2, 3},
        0u,
        lux::ecs::scene_format::EntitySectionId{
            uuid("aaaaaaaa-0000-4000-8000-000000000004")}});
    const auto duplicate_entry_result =
        canonical::encodeSceneCatalog(std::move(duplicate_entry));
    assert(!duplicate_entry_result &&
        duplicate_entry_result.error().error ==
            canonical::SceneCatalogError::DuplicateLocation);

    auto invalid_name = catalog;
    invalid_name.bands.front().source = canonical::SourceId{"Invalid Name"};
    assert(!canonical::encodeSceneCatalog(std::move(invalid_name)));

    auto nan_band = catalog;
    nan_band.bands.front().cell_world_size =
        std::numeric_limits<double>::quiet_NaN();
    assert(!canonical::encodeSceneCatalog(std::move(nan_band)));

    auto truncated = std::vector<std::byte>(
        kL3scV1Golden.begin(), kL3scV1Golden.end() - 1u);
    const auto truncated_result = canonical::decodeSceneCatalog(truncated);
    assert(!truncated_result && truncated_result.error().error ==
        canonical::SceneCatalogError::Truncated);

    auto trailing = std::vector<std::byte>(
        kL3scV1Golden.begin(), kL3scV1Golden.end());
    trailing.push_back(std::byte{0u});
    const auto trailing_result = canonical::decodeSceneCatalog(trailing);
    assert(!trailing_result && trailing_result.error().error ==
        canonical::SceneCatalogError::TrailingBytes);

    auto impossible_counts = std::vector<std::byte>(
        kL3scV1Golden.begin(), kL3scV1Golden.begin() + 40u);
    const std::uint32_t impossible_count = 4096u;
    std::memcpy(
        impossible_counts.data() + 8u,
        &impossible_count,
        sizeof(impossible_count));
    std::memcpy(
        impossible_counts.data() + 12u,
        &impossible_count,
        sizeof(impossible_count));
    const auto impossible = canonical::decodeSceneCatalog(impossible_counts);
    assert(!impossible && impossible.error().error ==
        canonical::SceneCatalogError::Truncated);

    const canonical::SceneCatalogCodecLimits tiny_limits{
        1u,
        1u,
        40u};
    assert(!canonical::decodeSceneCatalog(kL3scV1Golden, tiny_limits));
    return 0;
}
