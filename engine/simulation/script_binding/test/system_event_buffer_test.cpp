#include <lux/engine/simulation/SystemEventBuffer.hpp>

#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace
{
    struct OwnedPayload final
    {
        std::uint32_t value{};
        std::array<std::uint32_t, 8U> lifetime_canary{};
    };
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::task;

    SystemEventBuffer<OwnedPayload> buffer;
    assert(buffer.prepare(2U, 2U));
    auto first_writer = buffer.writer(0U);
    auto second_writer = buffer.writer(1U);
    assert(first_writer && second_writer);
    std::optional<SystemEventBuffer<OwnedPayload>::Writer> first_writer_state{
        std::move(*first_writer)};
    std::optional<SystemEventBuffer<OwnedPayload>::Writer> second_writer_state{
        std::move(*second_writer)};

    std::atomic_size_t callbacks{};
    TaskGraphBuilder builder;
    auto first = builder.add(
        [&first_writer_state]() noexcept
        {
            OwnedPayload first_payload{10U, {10U}};
            OwnedPayload second_payload{11U, {11U}};
            assert(first_writer_state->emit(
                lux::simulation::ecs::NullEntity,
                first_payload
            ));
            assert(first_writer_state->emit(
                lux::simulation::ecs::NullEntity,
                second_payload
            ));
            first_writer_state.reset();
        }
    );
    auto second = builder.add(
        [&second_writer_state]() noexcept
        {
            OwnedPayload payload{20U, {20U}};
            assert(second_writer_state->emit(
                lux::simulation::ecs::NullEntity,
                payload
            ));
            second_writer_state.reset();
        }
    );
    assert(first && second);
    auto safe = builder.add(
        on(ETaskAffinity::CALLER_THREAD),
        dependsOn(*first),
        dependsOn(*second),
        [&]() noexcept
        {
            std::vector<std::uint32_t> observed;
            observed.reserve(3U);
            assert(buffer.drain(
                [&](lux::simulation::ecs::Entity,
                    const OwnedPayload& payload) noexcept
                {
                    assert(payload.lifetime_canary[0] == payload.value);
                    observed.push_back(payload.value);
                    callbacks.fetch_add(1U, std::memory_order_relaxed);
                }
            ));
            assert((observed == std::vector<std::uint32_t>{10U, 11U, 20U}));
        }
    );
    assert(safe);
    auto graph = std::move(builder).build();
    assert(graph);
    TaskExecutor executor(TaskExecutorConfig{2U, graph->taskCount()});
    assert(callbacks.load(std::memory_order_relaxed) == 0U);
    assert(executor.execute(*graph));
    assert(callbacks.load(std::memory_order_relaxed) == 3U);
    assert(buffer.size() == 0U);

    auto overflow_writer = buffer.writer(0U);
    assert(overflow_writer);
    OwnedPayload payload{1U, {1U}};
    assert(overflow_writer->emit(lux::simulation::ecs::NullEntity, payload));
    assert(overflow_writer->emit(lux::simulation::ecs::NullEntity, payload));
    const auto overflow = overflow_writer->emit(
        lux::simulation::ecs::NullEntity,
        payload
    );
    assert(!overflow);
    assert(overflow.error() == ESystemEventBufferError::CAPACITY_EXCEEDED);
    overflow_writer = {};
    const auto failed_drain = buffer.drain(
        [](lux::simulation::ecs::Entity, const OwnedPayload&) noexcept {}
    );
    assert(!failed_drain);
    assert(failed_drain.error() == ESystemEventBufferError::RECORDING_FAILED);
    assert(buffer.reset());
}
