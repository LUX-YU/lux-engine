#pragma once

#include <atomic>
#include <lux/engine/scene/Observer.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <thread>

namespace run_test
{
inline std::atomic_uint64_t steps{}, destroyed_at_step{};
inline std::atomic_int64_t elapsed_ns{};
inline std::atomic_bool fail_step{};
inline std::atomic_uint32_t observer_partition{UINT32_MAX};
inline const auto main_thread = std::this_thread::get_id();

// Actual Simulation task, omitted in DERIVATION mode. No Editor or Render
// client is reachable here. Registry changes use its existing patch signals.
struct Motion final
{
    inline static constexpr auto Access =
        lux::simulation::makeSystemAccessSpec<lux::simulation::ComponentWrite<lux::simulation::ecs::Transform3D>,
                                              lux::simulation::ComponentWrite<lux::simulation::ecs::Light3D>,
                                              lux::simulation::ComponentWrite<lux::scene::Observer>>();
    inline static constexpr lux::simulation::SimulationSystemDescription Description{
        .type = {.canonical_name = "test.d3.motion", .version = 1}};

    lux::simulation::ecs::Registry &registry;
    const lux::simulation::SimulationClock &clock;
    ~Motion() noexcept
    {
        assert(std::this_thread::get_id() == main_thread);
        destroyed_at_step.store(clock.snapshot().step_index, std::memory_order_release);
    }

    bool advance() noexcept
    {
        assert(std::this_thread::get_id() == main_thread);
        using namespace lux::simulation::ecs;
        const auto snapshot = clock.snapshot();
        for (const auto observer : registry.view<lux::scene::Observer>())
        {
            const auto requested = observer_partition.load();
            const auto &partitions = registry.get<lux::scene::Observer>(observer).partitions;
            if ((requested == UINT32_MAX && !partitions.empty()) ||
                (requested != UINT32_MAX && (partitions.size() != 1 || partitions.front().value != requested)))
            {
                registry.patch<lux::scene::Observer>(observer, [requested](auto &value) {
                    value.partitions.clear();
                    if (requested != UINT32_MAX)
                    {
                        value.partitions.push_back({requested});
                    }
                });
            }
        }
        const auto entity = registry.view<Transform3D, Mesh3D>().front();
        assert(entity != NullEntity);
        registry.patch<Transform3D>(entity, [&](auto &transform) {
            transform.translation.x() += std::chrono::duration<double>(snapshot.delta).count();
        });
        // A bounded, observable backend count accompanies Transform evolution.
        // At most one extra light; no mesh/material streaming or new resources.
        if (snapshot.step_index % 2 && !registry.all_of<Light3D>(entity))
        {
            registry.emplace<Light3D>(entity);
        }
        else if (snapshot.step_index % 2 == 0)
        {
            registry.remove<Light3D>(entity);
        }
        elapsed_ns.store(snapshot.elapsed.count(), std::memory_order_relaxed);
        steps.store(snapshot.step_index, std::memory_order_release);
        return !fail_step.load(std::memory_order_acquire);
    }
};

inline lux::simulation::SimulationSystemRegistration registration()
{
    return {.type = lux::system::systemTypeId(Motion::Description.type.canonical_name),
            .cpp_type = lux::cxx::typeToken<Motion>(),
            .description = &Motion::Description,
            .access = Motion::Access.spec(),
            .install = [](lux::simulation::SimulationBuilder &builder,
                          lux::simulation::SimulationSystemView description) noexcept
                -> lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> {
                assert(std::this_thread::get_id() == main_thread);
                auto value =
                    builder.emplaceSystem<Motion>(description.instanceId(), builder.registry(), builder.clock());
                if (!value)
                {
                    return lux::cxx::unexpected(value.error());
                }
                return builder.addSystemTask<Motion>(description.instanceId(),
                                                     [](auto &motion) noexcept { return motion.advance(); });
            }};
}
} // namespace run_test
