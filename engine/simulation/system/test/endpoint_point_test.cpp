#include <lux/engine/simulation/EventPoint.hpp>
#include <lux/engine/simulation/HookPoint.hpp>

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace
{
    struct Target final
    {
        std::uint32_t index{};
        std::uint32_t generation{};
        friend bool operator==(Target, Target) noexcept = default;
    };

    void appendInt(void* context, std::int32_t value) noexcept
    {
        static_cast<std::vector<std::int32_t>*>(context)->push_back(value);
    }

    void appendBroadcast(void* context, const std::int32_t& value) noexcept
    {
        static_cast<std::vector<std::int32_t>*>(context)->push_back(value);
    }

    void appendTargeted(
        void* context,
        const Target& target,
        const std::int32_t& value
    ) noexcept
    {
        static_cast<std::vector<std::int32_t>*>(context)->push_back(
            static_cast<std::int32_t>(target.index * 100U) + value
        );
    }
}

int main()
{
    using namespace lux::simulation;

    HookPoint<void(std::int32_t)> hook;
    assert(hook.prepare(2U, 4U) == EEndpointMutationError::NONE);
    std::vector<std::int32_t> hook_values;
    const auto first = hook.connect(&hook_values, &appendInt);
    const auto second = hook.connect(&hook_values, &appendInt);
    assert(first && second);
    assert(hook.handlerCount() == 0U);
    assert(hook.flushMutations() == EEndpointMutationError::NONE);
    assert(hook.dispatch(7) == 2U);
    assert((hook_values == std::vector<std::int32_t>{7, 7}));
    assert(hook.disconnect(first.token) == EEndpointMutationError::NONE);
    assert(hook.flushMutations() == EEndpointMutationError::NONE);
    assert(hook.dispatch(8) == 1U);

    EventPoint<SimulationBroadcastRoute, std::int32_t> broadcast;
    static_assert(!std::is_move_constructible_v<decltype(broadcast)>);
    assert(broadcast.prepare(2U, 2U, 1U, 2U) ==
        EEndpointMutationError::NONE);
    std::vector<std::int32_t> broadcast_values;
    assert(broadcast.connect(&broadcast_values, &appendBroadcast));
    assert(broadcast.flushMutations() == EEndpointMutationError::NONE);
    {
        auto writer = broadcast.begin(1U);
        static_assert(!std::is_copy_constructible_v<decltype(writer)>);
        assert(writer.record(4));
        assert(writer.record(5));
        assert(broadcast.flushMutations() ==
            EEndpointMutationError::WRITER_ACTIVE);
        assert(broadcast.drain() == 0U);
    }
    assert(broadcast.drain() == 2U);
    assert((broadcast_values == std::vector<std::int32_t>{4, 5}));

    EventPoint<EntityTargetedRoute<Target>, std::int32_t> targeted;
    assert(targeted.prepare(2U, 2U, 2U, 2U) ==
        EEndpointMutationError::NONE);
    std::vector<std::int32_t> targeted_values;
    const Target old_entity{9U, 1U};
    const Target new_entity{9U, 2U};
    assert(targeted.connect(old_entity, &targeted_values, &appendTargeted));
    assert(targeted.connect(new_entity, &targeted_values, &appendTargeted));
    assert(targeted.flushMutations() == EEndpointMutationError::NONE);
    {
        auto producer_one = targeted.begin(1U);
        auto producer_zero = targeted.begin(0U);
        assert(producer_one.record(new_entity, 2));
        assert(producer_zero.record(old_entity, 1));
    }
    assert(targeted.drain() == 2U);
    assert((targeted_values == std::vector<std::int32_t>{901, 902}));
    return 0;
}
