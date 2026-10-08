#include <box2d/box2d.h>
#include <cassert>
#include <iostream>
#include <limits>
#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemInstaller.hpp>
#include <type_traits>

namespace
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::simulation;

    struct BackendWorlds final
    {
        std::vector<b2WorldId> values;

        BackendWorlds()
        {
            const auto definition = b2DefaultWorldDef();
            while (values.size() < 1024)
            {
                const auto id = b2CreateWorld(&definition);
                if (!b2World_IsValid(id))
                {
                    break;
                }
                values.push_back(id);
            }
            assert(!values.empty() && values.size() < 1024);
        }

        ~BackendWorlds()
        {
            for (auto id : values)
            {
                b2DestroyWorld(id);
            }
        }

        BackendWorlds(const BackendWorlds&) = delete;
        BackendWorlds& operator=(const BackendWorlds&) = delete;
    };

    std::size_t availableWorlds()
    {
        return BackendWorlds{}.values.size();
    }

    static_assert(!std::is_constructible_v<
                  Physics2DSystem,
                  ecs::Registry&,
                  const SimulationTime&,
                  Physics2DSystemConfiguration>);
    static_assert(!std::is_copy_constructible_v<Physics2DSystem> && !std::is_move_constructible_v<Physics2DSystem>);

    std::shared_ptr<const SimulationDescription> description(const Physics2DSystemConfiguration& configuration)
    {
        // Encode directly so the installer, not this helper, rejects semantically invalid configuration.
        const auto& registration = physics2DSystemRegistrations().front();
        std::vector<std::byte> encoded;
        assert(registration.configuration.encode(&configuration, encoded));
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(system::SystemInstanceId{1}, "physics", Physics2DSystem::Description, encoded));
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<SimulationDescription>(std::move(*built));
    }

    cxx::expected<void, SimulationSystemBuildFailure> installAndCheckRejection(
        SimulationSystemInstaller& builder,
        SimulationSystemView view
    ) noexcept
    {
        auto installed = physics2DSystemRegistrations().front().install(builder, view);
        if (!installed)
        {
            return installed;
        }
        auto candidate = Physics2DSystem::create(builder.registry(), builder.time(), {.body_capacity = 4});
        assert(candidate);
        const auto* before = candidate->get();
        auto rejected = builder.addSystem(view.instanceId(), std::move(*candidate));
        assert(!rejected && rejected.error().code == ESimulationSystemBuildError::DUPLICATE_SYSTEM);
        assert(candidate->get() == before);
        rejected = builder.addSystem(system::SystemInstanceId{2}, std::move(*candidate));
        assert(!rejected && rejected.error().code == ESimulationSystemBuildError::INVALID_DESCRIPTION);
        assert(candidate->get() == before);
        return {};
    }
} // namespace

int main()
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::simulation;
    ecs::Registry registry;
    SimulationTime time;
    const auto capacity = availableWorlds();
    Physics2DSystemConfiguration invalid;
    invalid.fixed_step_nanoseconds = 0;
    auto rejected = Physics2DSystem::create(registry, time, invalid);
    assert(!rejected && rejected.error() == EPhysics2DSystemError::INVALID_CONFIGURATION);
    assert(availableWorlds() == capacity);
    invalid = {};
    invalid.body_capacity = 0;
    assert(!Physics2DSystem::create(registry, time, invalid));
    invalid = {};
    invalid.gravity_y = std::numeric_limits<double>::quiet_NaN();
    assert(!Physics2DSystem::create(registry, time, invalid));
    invalid = {};
    invalid.fixed_step_nanoseconds = std::numeric_limits<std::int64_t>::max();
    assert(!Physics2DSystem::create(registry, time, invalid));
    assert(availableWorlds() == capacity);
    {
        const BackendWorlds saturated;
        rejected = Physics2DSystem::create(registry, time, {.body_capacity = 4});
        assert(!rejected && rejected.error() == EPhysics2DSystemError::CAPACITY_EXCEEDED);
    }
    assert(availableWorlds() == capacity);
    {
        const auto entity = registry.create();
        registry.emplace<ecs::Transform2D>(entity).translation = {1.0e9, 1.0e9};
        registry.emplace<BoxCollider2D>(entity);
        registry.emplace<RigidBody2D>(entity);
        auto system = Physics2DSystem::create(registry, time, {.body_capacity = 4});
        assert(system && (*system)->stats().active_bodies == 0);
        time.delta = SimulationDuration{16'666'667};
        assert((*system)->update());
        assert((*system)->stats().active_bodies == 1 && (*system)->stats().completed_steps == 1);
        assert(registry.get<ecs::Transform2D>(entity).translation.y() < 1.0e9);
        assert((*system)->overlapsBox(1.0e9, 1.0e9, 1, 1));
        assert(!(*system)->overlapsBox(1.0e9, 1.0e9, -1, 1));
        registry.destroy(entity);
        time.delta = {};
        assert((*system)->update() && (*system)->stats().active_bodies == 0);
    }
    assert(availableWorlds() == capacity);
    SimulationSystemRegistry types;
    auto registration = physics2DSystemRegistrations().front();
    registration.install = installAndCheckRejection;
    assert(types.add(registration));
    {
        auto invalid_description = description(invalid);
        auto failed = Simulation::create(registry, invalid_description, types);
        assert(!failed && failed.error().code == ESimulationSystemBuildError::CONFIGURATION_DECODE_FAILURE);
        assert(availableWorlds() == capacity);
        auto valid = description({.body_capacity = 4});
        {
            const BackendWorlds saturated;
            failed = Simulation::create(registry, valid, types);
            assert(!failed && failed.error().code == ESimulationSystemBuildError::CONSTRUCTION_FAILURE);
        }
        auto simulation = Simulation::create(registry, valid, types);
        assert(simulation && simulation->taskCount() == 1 && !simulation->scriptApiCapabilities().empty());
        assert(simulation->seal());
        auto executor = task::TaskExecutor::create({0, 1024});
        assert(executor);
        const auto bad_time =
            simulation->execute(*executor, {.delta = SimulationDuration{16'666'667}, .step_index = 1});
        assert(!bad_time && bad_time.error().code == ESimulationExecutionError::INVALID_STEP_TIME);
        const SimulationTime next{SimulationDuration{16'666'667}, SimulationDuration{16'666'667}, 1};
        assert(simulation->execute(*executor, next));
    }
    assert(availableWorlds() == capacity);
    std::cout << "physics2d: invalid input owns no backend, real capacity failure/recovery, stepping, queries, "
                 "complete installation and rejection retains candidate PASS\n";
}
