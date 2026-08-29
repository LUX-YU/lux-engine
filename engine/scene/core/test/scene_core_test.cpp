#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }
}

int main()
{
    using namespace lux;

    static_assert(!std::is_copy_constructible_v<scene::Scene>);
    static_assert(!std::is_move_constructible_v<scene::Scene>);

    world::WorldDescriptionBuilder world_builder;
    assert(world_builder.setIdentity(
        id<world::WorldBundleId>(1U),
        id<world::WorldBundleGeneration>(2U),
        "scene-test"
    ));
    assert(world_builder.setPartitioner({world::worldPartitionerId("test.none"), 1U}, 0U));
    auto world = std::move(world_builder).build();
    assert(world);

    simulation::SimulationDescriptionBuilder simulation_builder;
    auto simulation = std::move(simulation_builder).build();
    assert(simulation);

    simulation::SystemRegistry systems;
    auto scene = scene::Scene::create(
        std::make_shared<world::WorldDescription>(std::move(*world)),
        std::make_shared<simulation::SimulationDescription>(std::move(*simulation)),
        systems
    );
    assert(scene);
    assert(!(*scene)->stopToken().stop_requested());
    (*scene)->requestStop();
    assert((*scene)->stopToken().stop_requested());
    (*scene)->requestStop();

    assert(!scene::Scene::create({}, {}, systems));
    return 0;
}
