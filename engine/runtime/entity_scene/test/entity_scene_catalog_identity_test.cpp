#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>

#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/scene/SceneFeatureId.hpp>

#include <cassert>
#include <concepts>
#include <utility>

namespace
{
    template <class Id>
    concept CatalogLookupId = requires(
        const lux::runtime::entity_scene::EntitySceneCatalog& catalog,
        Id id)
    {
        catalog.findContribution(id);
    };
}

int main()
{
    static_assert(CatalogLookupId<lux::scene::SceneFeatureIdView>);
    static_assert(!CatalogLookupId<lux::extensions::ContributionIdView>);

    const auto parsed = uuids::uuid::from_string(
        "71000000-0000-4000-8000-000000000001");
    assert(parsed);

    lux::entity_scene::EntitySceneManifest manifest;
    manifest.id = lux::entity_scene::EntitySceneId{*parsed};
    manifest.contributions.push_back({
        lux::extensions::ContributionId{"org.lux.test.catalog_identity"},
        1u,
        {}});

    auto catalog =
        lux::runtime::entity_scene::EntitySceneCatalog::create(
            std::move(manifest));
    assert(catalog);
    assert(catalog->findContribution(
        lux::scene::sceneFeatureId(
            "org.lux.test.catalog_identity")) != nullptr);
    assert(catalog->findContribution(
        lux::scene::sceneFeatureId(
            "org.lux.test.catalog_missing")) == nullptr);
    return 0;
}
