#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lux::simulation::test
{
    struct LUX_TYPE_INFO(runtime) BridgeRecord final
    {
        std::int32_t value{};
    };

    inline ecs::Entity observed_self{ecs::NullEntity};
    inline float observed_value{};
    inline std::int32_t observed_record{};
    inline std::int32_t observed_lifecycle_value{};
    inline lux::simulation::script::EScriptEndPlayReason observed_end_reason{};
    inline std::size_t constructed_objects{};
    inline std::size_t destroyed_objects{};
    inline std::int32_t coroutine_value{};
    inline std::int32_t coroutine_event_value{};
    inline std::optional<script::CppScriptEventSource<std::int32_t>> coroutine_event_source;

    class LUX_TYPE_INFO(runtime) BridgeBehavior final
    {
    public:
        BridgeBehavior() noexcept
        {
            ++constructed_objects;
        }

        ~BridgeBehavior() noexcept
        {
            ++destroyed_objects;
        }

        LUX_METHOD()
        void admitToGameplay() noexcept
        {
            lifecycle_value_ = 10;
        }

        LUX_METHOD()
        void onValue(float value) noexcept
        {
            observed_value = value;
            ++lifecycle_value_;
        }

        LUX_METHOD()
        void onRecord(const BridgeRecord& value) noexcept
        {
            observed_record = value.value;
        }

        LUX_METHOD()
        void leaveGameplay(lux::simulation::script::EScriptEndPlayReason reason) noexcept
        {
            observed_lifecycle_value = lifecycle_value_;
            observed_end_reason = reason;
        }

        LUX_METHOD()
        void throwing(float)
        {
        }

        LUX_METHOD()
        script::ScriptCoroutine task(
            script::ScriptCoroutineContext& context,
            const std::int32_t& borrowed_value
        ) noexcept
        {
            const auto owned_value = borrowed_value;
            coroutine_value += owned_value;
            co_await context.makeAwaiter<void>(
                [](script::ScriptCoroutineContext&, script::ScriptStepContext& step) noexcept
                {
                    const auto waiting = step.awaitables.create();
                    return waiting
                        ? script::ScriptStepResult::suspended(waiting->id)
                        : script::ScriptStepResult::failed(-1);
                }
            );
            coroutine_value += owned_value + 5;
        }

        LUX_METHOD()
        script::ScriptCoroutine waitForEvent(script::ScriptCoroutineContext& context) noexcept
        {
            if (!coroutine_event_source)
                co_return;
            coroutine_event_value = co_await context.wait(*coroutine_event_source);
        }

        LUX_METHOD()
        script::ScriptCoroutine waitForNextStep(script::ScriptCoroutineContext& context) noexcept
        {
            co_await context.makeAwaiter<void>(
                [](script::ScriptCoroutineContext&, script::ScriptStepContext& step) noexcept
                {
                    const auto waiting = step.awaitables.create();
                    return waiting
                        ? script::ScriptStepResult::suspended(waiting->id)
                        : script::ScriptStepResult::failed(-1);
                }
            );
            coroutine_value += 100;
        }

        LUX_METHOD()
        script::ScriptCoroutine waitTwice(script::ScriptCoroutineContext& context) noexcept
        {
            co_await context.makeAwaiter<void>(
                [](script::ScriptCoroutineContext&, script::ScriptStepContext& step) noexcept
                {
                    const auto waiting = step.awaitables.create();
                    return waiting
                        ? script::ScriptStepResult::suspended(waiting->id)
                        : script::ScriptStepResult::failed(-1);
                }
            );
            coroutine_value += 1'000;
            co_await context.makeAwaiter<void>(
                [](script::ScriptCoroutineContext&, script::ScriptStepContext& step) noexcept
                {
                    const auto waiting = step.awaitables.create();
                    return waiting
                        ? script::ScriptStepResult::suspended(waiting->id)
                        : script::ScriptStepResult::failed(-1);
                }
            );
            coroutine_value += 10'000;
        }

        void unmarkedHelper() noexcept
        {
        }


    private:
        std::int32_t lifecycle_value_{};
    };

    inline std::int32_t LUX_FUNC() bridgeFreeFunction(std::int32_t value) noexcept
    {
        return value + 1;
    }
}
