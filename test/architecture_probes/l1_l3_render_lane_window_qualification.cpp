#include "DeviceRenderFixture.hpp"

#include <lux/engine/scene/LatestSpscExchange.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/systems/TransformSystem.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct PresentationState final
    {
        Eigen::Vector3d translation{};
        std::uint64_t revision{};
    };

    [[nodiscard]] std::shared_ptr<const lux::simulation::SimulationDescription> makeDescription()
    {
        using namespace lux::simulation;
        const auto configuration = makeTransformSystemConfiguration(
            64U,
            lux::simulation::ecs::EcsCommandProducerCapacity{128U, 64U * 1024U}
        );
        if (!configuration)
        {
            return {};
        }
        SimulationDescriptionBuilder builder;
        if (!builder.addSystem(SystemInstanceId{1U}, "transform", transformSystemDescription(), *configuration))
        {
            return {};
        }
        auto description = std::move(builder).build();
        return description ? std::make_shared<SimulationDescription>(std::move(*description)) : nullptr;
    }
}

int main()
{
    using namespace lux;
    constexpr int kSkip = 77;

    std::atomic_int validation_errors{};
    rendertest::DeviceRenderFixture fixture(
        640U,
        360U,
        "l1_l3_render_lane_window_qualification",
        {.enable_validation = true, .validation_errors = &validation_errors}
    );
    if (!fixture.ok())
    {
        std::puts("SKIP: Vulkan window or validation layer unavailable");
        return kSkip;
    }
    (void)fixture.makeSceneWithView("WindowLaneScene", "WindowLaneView");

    simulation::SystemRegistry system_types;
    if (!system_types.add(simulation::transformSystemRegistrations()))
    {
        return 2;
    }
    simulation::ecs::Registry registry;
    const auto entity = registry.create();
    registry.emplace<simulation::ecs::Transform3D>(entity);
    auto simulation = simulation::Simulation::create(registry, makeDescription(), system_types);
    auto executor = task::TaskExecutor::create(task::TaskExecutorConfig{0U, 64U});
    if (!simulation || !executor)
    {
        return 3;
    }

    scene::LatestSpscExchange<PresentationState> exchange;
    std::atomic_uint64_t simulation_steps{};
    std::atomic_bool simulation_failed{};
    std::jthread simulation_thread([&](std::stop_token stop) {
        std::uint64_t revision{};
        auto next = Clock::now();
        while (!stop.stop_requested())
        {
            ++revision;
            registry.patch<simulation::ecs::Transform3D>(entity, [revision](auto& transform) {
                transform.translation = Eigen::Vector3d{
                    std::sin(static_cast<double>(revision) * 0.1),
                    0.0,
                    -2.0
                };
            });
            if (!simulation->execute(*executor))
            {
                simulation_failed.store(true, std::memory_order_relaxed);
                return;
            }
            exchange.write() = PresentationState{
                registry.get<simulation::ecs::WorldTransform3D>(entity).value.translation(),
                revision
            };
            exchange.publish();
            simulation_steps.store(revision, std::memory_order_relaxed);
            next += std::chrono::milliseconds(50);
            std::this_thread::sleep_until(next);
        }
    });

    const auto started = Clock::now();
    const auto deadline = started + std::chrono::milliseconds(1100);
    std::uint64_t acquired_revision{};
    std::uint64_t steps_before_pause{};
    std::uint64_t steps_after_pause{};
    std::uint64_t presentation_frames{};
    while (Clock::now() < deadline)
    {
        const auto elapsed = Clock::now() - started;
        const bool forwarding_paused = elapsed >= std::chrono::milliseconds(400) &&
            elapsed < std::chrono::milliseconds(600);
        fixture.window().pollEvents();
        fixture.session().pumpReplies();
        fixture.control().pumpReplies();
        fixture.upload().pumpReplies();
        if (!forwarding_paused && exchange.acquireLatest())
        {
            acquired_revision = exchange.read().revision;
        }
        if (elapsed >= std::chrono::milliseconds(390) && steps_before_pause == 0U)
        {
            steps_before_pause = simulation_steps.load(std::memory_order_relaxed);
        }
        if (elapsed >= std::chrono::milliseconds(610) && steps_after_pause == 0U)
        {
            steps_after_pause = simulation_steps.load(std::memory_order_relaxed);
        }
        if (fixture.session().trySubmitFrame() && fixture.session().beginFrame({}))
        {
            ++presentation_frames;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    simulation_thread.request_stop();
    simulation_thread.join();
    if (exchange.acquireLatest())
    {
        acquired_revision = exchange.read().revision;
    }
    const auto final_steps = simulation_steps.load(std::memory_order_relaxed);
    const bool simulation_continued_during_pause = steps_after_pause > steps_before_pause;
    const bool latest_visible = acquired_revision == final_steps;
    std::printf(
        "simulation_steps=%llu,presentation_frames=%llu,pause_before=%llu,pause_after=%llu,latest=%llu," 
        "validation_errors=%d\n",
        static_cast<unsigned long long>(final_steps),
        static_cast<unsigned long long>(presentation_frames),
        static_cast<unsigned long long>(steps_before_pause),
        static_cast<unsigned long long>(steps_after_pause),
        static_cast<unsigned long long>(acquired_revision),
        validation_errors.load()
    );
    return !simulation_failed.load() && simulation_continued_during_pause && latest_visible &&
            presentation_frames > final_steps && validation_errors.load() == 0
        ? 0
        : 4;
}
