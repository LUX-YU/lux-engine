#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>

#include <cassert>
#include <utility>

int main()
{
    const auto parsed = uuids::uuid::from_string(
        "71000000-0000-4000-8000-000000000001");
    assert(parsed);

    lux::scene::SceneDescription package;
    package.id = lux::asset::asset_id_t{*parsed};
    auto catalog =
        lux::runtime::entity_scene::EntitySceneCatalog::create(
            std::move(package));
    assert(catalog);
    assert(catalog->findSection(
        lux::ecs::scene_format::EntitySectionId{*parsed}) == nullptr);
    return 0;
}
