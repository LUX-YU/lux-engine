#include <lux/engine/resource/spatial3d_scene/Spatial3DSceneCatalog.hpp>
#include <lux/engine/spatial3d/SceneCatalog.hpp>

#include <cassert>
#include <type_traits>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }
}

int main()
{
    namespace canonical = lux::spatial3d;
    namespace legacy = lux::spatial3d_scene;

    static_assert(!std::is_same_v<
        canonical::SceneCatalog,
        legacy::Spatial3DSceneCatalogConfig>);
    static_assert(!std::is_convertible_v<
        legacy::Spatial3DSceneCatalogConfig,
        canonical::SceneCatalog>);

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

    legacy::Spatial3DSceneCatalogConfig old;
    old.residency = {4096u, 128u, 4u, 16u};
    old.bands.push_back({
        legacy::Spatial3DSourceId{"lux.spatial3d.source.test"},
        lux::entity_scene::DemandChannelId{
            canonical::kResidentDemandChannelName},
        0u,
        64.0,
        1.0,
        1.5});
    old.entries.push_back({
        {1, 2, 3},
        0u,
        lux::entity_scene::EntitySectionId{
            uuid("aaaaaaaa-0000-4000-8000-000000000001")}});

    const auto encoded = canonical::encodeSceneCatalog(catalog);
    const auto legacy_encoded = legacy::encodeSpatial3DSceneCatalog(old);
    assert(encoded && legacy_encoded);
    assert(*encoded == *legacy_encoded);

    const auto decoded = canonical::decodeSceneCatalog(*legacy_encoded);
    const auto legacy_decoded = legacy::decodeSpatial3DSceneCatalog(*encoded);
    assert(decoded && legacy_decoded);
    assert(*decoded == catalog);
    assert(*legacy_decoded == old);
    return 0;
}
