#include <lux/engine/physics2d/Physics2DSystem.hpp>

#include "PhysicsQuery2D.ability.generated.hpp"
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <cassert>
#include <chrono>
#include <memory>

namespace
{
    constexpr lux::system::SystemInstanceId kPhysics{0x502D0001U};

    [[nodiscard]] std::shared_ptr<const lux::simulation::SimulationDescription> description()
    {
        lux::physics2d::Physics2DSystemConfiguration configuration;
        configuration.gravity_y = -10.0;
        configuration.fixed_step_nanoseconds = 100'000'000;
        configuration.max_substeps = 4U;
        configuration.body_capacity = 8U;
        const auto encoded = lux::physics2d::makePhysics2DSystemConfiguration(configuration);
        assert(encoded);
        lux::simulation::SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kPhysics, "physics2d", lux::physics2d::physics2DSystemDescription(), *encoded));
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<lux::simulation::SimulationDescription>(std::move(*built));
    }
}

int main()
{
    using namespace std::chrono_literals;
    using namespace lux::physics2d;
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;

    SimulationSystemRegistry systems;
    assert(systems.add(physics2DSystemRegistrations()));
    Registry registry;
    const auto floor = registry.create();
    registry.emplace<Transform2D>(floor, Transform2D{Eigen::Vector2d{0.0, 0.0}, 0.0, Eigen::Vector2d::Ones()});
    registry.emplace<BoxCollider2D>(floor, BoxCollider2D{Eigen::Vector2d{2.0, 0.5}, Eigen::Vector2d::Zero()});

    const auto falling = registry.create();
    registry.emplace<Transform2D>(falling, Transform2D{Eigen::Vector2d{0.0, 5.0}, 0.0, Eigen::Vector2d::Ones()});
    registry.emplace<BoxCollider2D>(falling);
    registry.emplace<RigidBody2D>(falling);

    auto simulation = Simulation::create(registry, description(), systems);
    assert(simulation);
    auto executor = lux::task::TaskExecutor::create({0U, 1U});
    assert(executor);
    assert(simulation->execute(*executor, SimulationDuration{}));

    const auto capabilities = simulation->scriptApiCapabilities();
    assert(capabilities.size() == 1U);
    using Traits = lux::script::ScriptAbilityTraits<PhysicsQuery2D>;
    assert(capabilities.front().contract == Traits::Description.id);
    assert(capabilities.front().schema_hash == Traits::Description.schema_hash);
    auto* provider = static_cast<Physics2DSystem*>(capabilities.front().context);
    assert(provider != nullptr);
    assert(provider->overlapsBox(0.0, 0.0, 0.25, 0.25));
    const lux::script::ScriptAbilityBinding prepared_binding{&Traits::Description,
                                                             capabilities.front().context,
                                                             capabilities.front().dispatch,
                                                             capabilities.front().methods};
    const auto prepared_api = lux::script::ScriptAbilityCpp<PhysicsQuery2D>::create(prepared_binding);
    assert(prepared_api && prepared_api->overlapsBox(0.0, 0.0, 0.25, 0.25));
    assert(!provider->overlapsBox(50.0, 50.0, 0.25, 0.25));
    assert(!provider->overlapsBox(0.0, 0.0, -1.0, 1.0));

    const auto before = registry.get<Transform2D>(falling).translation.y();
    assert(simulation->execute(*executor, std::chrono::duration_cast<SimulationDuration>(100ms)));
    const auto after = registry.get<Transform2D>(falling).translation.y();
    assert(after < before);
    assert(simulation->execute(*executor, SimulationDuration{}));
    assert(registry.get<Transform2D>(falling).translation.y() == after);

    registry.destroy(floor);
    assert(simulation->execute(*executor, SimulationDuration{}));
    assert(!provider->overlapsBox(0.0, 0.0, 0.25, 0.25));
    return 0;
}
