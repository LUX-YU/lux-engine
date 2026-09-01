#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <memory>

namespace
{
    template <class Type>
    [[nodiscard]] Type worldId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId(bytes);
    }
}

int main()
{
    using namespace lux;

    world::WorldDescriptionBuilder world_builder;
    if (!world_builder.setIdentity(
            worldId<world::WorldBundleId>(1U),
            worldId<world::WorldBundleGeneration>(2U),
            "dedicated") ||
        !world_builder.setPartitioner({world::worldPartitionerId("dedicated.none"), 1U}, 0U))
    {
        return 1;
    }
    auto world = std::move(world_builder).build();
    if (!world) return 2;

    simulation::SimulationDescriptionBuilder simulation_builder;
    auto simulation = std::move(simulation_builder).build();
    if (!simulation) return 3;

    scene::SceneDescriptionBuilder scene_builder;
    scene_builder.setWorld(assetId(1U));
    scene_builder.setSimulation(assetId(2U));
    auto description = std::move(scene_builder).build();
    if (!description) return 4;

    meta::ReflectionRegistry::initRegistry();
    auto components = simulation::ecs::ComponentSchemaSet::build({});
    if (!components) return 5;
    auto scene_meta = scene::SceneMetaManager::build({
        std::move(*components),
        simulation::SimulationSystemRegistry{}
    });
    if (!scene_meta) return 6;
    auto created = scene::Scene::create({
        std::make_shared<scene::SceneDescription>(std::move(*description)),
        std::make_shared<world::WorldDescription>(std::move(*world)),
        std::make_shared<simulation::SimulationDescription>(std::move(*simulation)),
        *scene_meta,
        {}
    });
    if (!created || (*created)->hasCapability("lux.scene.render")) return 7;
    created->reset();
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
