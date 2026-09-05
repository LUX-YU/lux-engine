#include "HookChannelTestDriver.hpp"
#include <lux/engine/simulation/HookChannel.hpp>
#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace
{
    using lux::simulation::EEndpointMutationError;
    using lux::simulation::EndpointConnectionToken;

    struct HookProbe final
    {
        lux::simulation::HookPoint<void(std::int32_t)> *endpoint{};
        EndpointConnectionToken token;
        EEndpointMutationError disconnect_error{EEndpointMutationError::NONE};
        EEndpointMutationError prepare_error{EEndpointMutationError::NONE};
        std::vector<std::int32_t> values;
    };

    void appendInt(void *context, std::int32_t value) noexcept
    {
        static_cast<std::vector<std::int32_t> *>(context)->push_back(value);
    }

    void incrementSize(void *context, const std::size_t &) noexcept
    {
        ++*static_cast<std::size_t *>(context);
    }

    void probeHookMutation(void *context, std::int32_t value) noexcept
    {
        auto &probe = *static_cast<HookProbe *>(context);
        probe.values.push_back(value);
        probe.disconnect_error = probe.endpoint->disconnect(probe.token);
        probe.prepare_error = probe.endpoint->prepare(4U);
    }

    struct BroadcastProbe final
    {
        using Channel = lux::simulation::test::HookChannelTestDriver<
            lux::simulation::SimulationBroadcastRoute, std::int32_t>;
        Channel* endpoint{};
        EndpointConnectionToken token;
        EEndpointMutationError disconnect_error{EEndpointMutationError::NONE};
        EEndpointMutationError prepare_error{EEndpointMutationError::NONE};
        std::vector<std::int32_t> values;
    };

    void probeBroadcastMutation(void *context, const std::int32_t &value) noexcept
    {
        auto &probe = *static_cast<BroadcastProbe *>(context);
        probe.values.push_back(value);
        probe.disconnect_error = probe.endpoint->disconnect(probe.token);
        probe.prepare_error = probe.endpoint->prepare(2U, 2U, 2U);
    }

    using TargetedEvent =
        lux::simulation::test::HookChannelTestDriver<
            lux::simulation::EntityTargetedRoute<lux::simulation::ecs::Entity>, std::int32_t>;

    struct TargetedProbe final
    {
        std::vector<std::int32_t> values;
        std::int32_t prefix{};
    };

    void appendTargeted(
        void *context,
        const lux::simulation::ecs::Entity &target,
        const std::int32_t &value) noexcept
    {
        auto &probe = *static_cast<TargetedProbe *>(context);
        const auto index = static_cast<std::int32_t>(entt::to_entity(target));
        probe.values.push_back(probe.prefix + index * 100 + value);
    }

    [[nodiscard]] constexpr lux::simulation::ecs::Entity makeEntity(
        std::uint32_t index,
        std::uint32_t generation) noexcept
    {
        return entt::entt_traits<lux::simulation::ecs::Entity>::construct(index, generation);
    }
}

int main()
{
    using namespace lux::simulation;

    HookPoint<void(std::int32_t)> hook;
    assert(hook.prepare(2U) == EEndpointMutationError::NONE);

    std::vector<std::int32_t> hook_values;
    const auto first = hook.connect(&hook_values, &appendInt);
    assert(first);
    assert(hook.handlerCount() == 1U);
    assert(hook.dispatch(7) == 1U);
    assert((hook_values == std::vector<std::int32_t>{7}));
    assert(hook.disconnect(first.token) == EEndpointMutationError::NONE);
    assert(hook.disconnect(first.token) == EEndpointMutationError::INVALID_TOKEN);

    const auto reused = hook.connect(&hook_values, &appendInt);
    assert(reused);
    assert(reused.token.slot == first.token.slot);
    assert(reused.token.generation != first.token.generation);
    assert(hook.disconnect(first.token) == EEndpointMutationError::INVALID_TOKEN);
    assert(hook.disconnect(reused.token) == EEndpointMutationError::NONE);

    HookProbe hook_probe{&hook};
    const auto probe_connection = hook.connect(&hook_probe, &probeHookMutation);
    assert(probe_connection);
    hook_probe.token = probe_connection.token;
    assert(hook.dispatch(8) == 1U);
    assert(hook_probe.disconnect_error == EEndpointMutationError::DISPATCH_ACTIVE);
    assert(hook_probe.prepare_error == EEndpointMutationError::DISPATCH_ACTIVE);
    assert(hook.disconnect(probe_connection.token) == EEndpointMutationError::NONE);

    lux::simulation::test::HookChannelTestDriver<SimulationBroadcastRoute, std::int32_t> broadcast;
    static_assert(!std::is_move_constructible_v<decltype(broadcast)>);
    assert(broadcast.prepare(2U, 2U, 1U) == EEndpointMutationError::NONE);

    BroadcastProbe broadcast_probe{&broadcast};
    const auto broadcast_connection = broadcast.connect(&broadcast_probe, &probeBroadcastMutation);
    assert(broadcast_connection);
    broadcast_probe.token = broadcast_connection.token;
    {
        auto writer = broadcast.begin(1U);
        static_assert(!std::is_copy_constructible_v<decltype(writer)>);
        static_assert(std::is_move_constructible_v<decltype(writer)>);
        assert(writer.record(4));
        assert(writer.record(5));
        assert(broadcast.prepare(2U, 2U, 1U) == EEndpointMutationError::WRITER_ACTIVE);
        assert(broadcast.disconnect(broadcast_connection.token) == EEndpointMutationError::WRITER_ACTIVE);
        assert(broadcast.activate() == 0U);
    }
    assert(broadcast.activate() == 2U);
    assert((broadcast_probe.values == std::vector<std::int32_t>{4, 5}));
    assert(broadcast_probe.disconnect_error == EEndpointMutationError::DISPATCH_ACTIVE);
    assert(broadcast_probe.prepare_error == EEndpointMutationError::DISPATCH_ACTIVE);
    assert(broadcast.disconnect(broadcast_connection.token) == EEndpointMutationError::NONE);

    TargetedEvent targeted;
    assert(targeted.prepare(2U, 2U, 4U) == EEndpointMutationError::NONE);

    TargetedProbe exact_probe;
    TargetedProbe all_probe{{}, 10000};
    const auto old_entity = makeEntity(9U, 1U);
    const auto new_entity = makeEntity(9U, 2U);
    const auto old_connection = targeted.connect(old_entity, &exact_probe, &appendTargeted);
    const auto all_connection = targeted.connectAll(&all_probe, &appendTargeted);
    assert(old_connection && all_connection);
    assert(targeted.targetBucketCount() == 1U);
    {
        auto producer_zero = targeted.begin(0U);
        assert(producer_zero.record(old_entity, 1));
    }
    assert(targeted.activate() == 2U);
    assert((exact_probe.values == std::vector<std::int32_t>{901}));
    assert((all_probe.values == std::vector<std::int32_t>{10901}));

    assert(targeted.disconnect(old_connection.token) == EEndpointMutationError::NONE);
    assert(targeted.targetBucketCount() == 0U);
    assert(targeted.disconnect(old_connection.token) == EEndpointMutationError::INVALID_TOKEN);

    const auto new_connection = targeted.connect(new_entity, &exact_probe, &appendTargeted);
    assert(new_connection);
    assert(new_connection.token.slot == old_connection.token.slot);
    assert(new_connection.token.generation != old_connection.token.generation);
    TargetedProbe removed_probe{{}, 20000};
    TargetedProbe moved_probe{{}, 30000};
    const auto removed_connection = targeted.connect(new_entity, &removed_probe, &appendTargeted);
    const auto moved_connection = targeted.connect(new_entity, &moved_probe, &appendTargeted);
    assert(removed_connection && moved_connection);
    assert(targeted.disconnect(removed_connection.token) == EEndpointMutationError::NONE);
    assert(targeted.disconnect(removed_connection.token) == EEndpointMutationError::INVALID_TOKEN);
    {
        auto producer_zero = targeted.begin(0U);
        assert(producer_zero.record(old_entity, 3));
        assert(producer_zero.record(new_entity, 2));
    }
    assert(targeted.activate() == 4U);
    assert((exact_probe.values == std::vector<std::int32_t>{901, 902}));
    assert((all_probe.values == std::vector<std::int32_t>{10901, 10903, 10902}));
    assert(removed_probe.values.empty());
    assert((moved_probe.values == std::vector<std::int32_t>{30902}));

    assert(targeted.disconnect(new_connection.token) == EEndpointMutationError::NONE);
    assert(targeted.disconnect(moved_connection.token) == EEndpointMutationError::NONE);
    assert(targeted.disconnect(all_connection.token) == EEndpointMutationError::NONE);
    assert(targeted.handlerCount() == 0U);
    assert(targeted.targetBucketCount() == 0U);

    constexpr std::size_t kConcurrentProducerCount = 4U;
    constexpr std::size_t kConcurrentEpochCount = 256U;
    lux::simulation::test::HookChannelTestDriver<SimulationBroadcastRoute, std::size_t> concurrent;
    const auto concurrent_prepare = concurrent.prepare(kConcurrentProducerCount, kConcurrentEpochCount, 1U);
    assert(concurrent_prepare == EEndpointMutationError::NONE);
    std::size_t concurrent_callbacks{};
    const auto concurrent_connection = concurrent.connect(&concurrent_callbacks, &incrementSize);
    assert(concurrent_connection);

    std::barrier writers_started{static_cast<std::ptrdiff_t>(kConcurrentProducerCount)};
    std::barrier mutation_checked{static_cast<std::ptrdiff_t>(kConcurrentProducerCount)};
    std::barrier writers_released{static_cast<std::ptrdiff_t>(kConcurrentProducerCount)};
    std::atomic_bool concurrent_failure{};
    lux::task::TaskGraphBuilder concurrent_builder;
    for (std::size_t producer{}; producer < kConcurrentProducerCount; ++producer)
    {
        const auto added = concurrent_builder.add([&, producer]() noexcept
        {
            for (std::size_t epoch{}; epoch < kConcurrentEpochCount; ++epoch)
            {
                {
                    auto writer = concurrent.begin(producer);
                    if (!writer.record(producer * kConcurrentEpochCount + epoch))
                        concurrent_failure.store(true, std::memory_order_relaxed);

                    writers_started.arrive_and_wait();
                    if (producer == 0U)
                    {
                        const auto prepare_error = concurrent.prepare(
                            kConcurrentProducerCount,
                            kConcurrentEpochCount,
                            1U
                        );
                        if (prepare_error != EEndpointMutationError::WRITER_ACTIVE || concurrent.activate() != 0U)
                            concurrent_failure.store(true, std::memory_order_relaxed);
                    }
                    mutation_checked.arrive_and_wait();
                }
                writers_released.arrive_and_wait();
            }
        });
        assert(added);
    }
    auto concurrent_graph = std::move(concurrent_builder).build();
    assert(concurrent_graph);
    auto concurrent_executor = lux::task::TaskExecutor::create(
        lux::task::TaskExecutorConfig{
            static_cast<std::uint32_t>(kConcurrentProducerCount),
            concurrent_graph->taskCount()
        }
    );
    assert(concurrent_executor);
    assert(concurrent_executor->execute(*concurrent_graph));
    assert(!concurrent_failure.load(std::memory_order_relaxed));
    assert(concurrent.activate() == kConcurrentProducerCount * kConcurrentEpochCount);
    assert(concurrent_callbacks == kConcurrentProducerCount * kConcurrentEpochCount);
    assert(concurrent.disconnect(concurrent_connection.token) == EEndpointMutationError::NONE);
    return 0;
}
