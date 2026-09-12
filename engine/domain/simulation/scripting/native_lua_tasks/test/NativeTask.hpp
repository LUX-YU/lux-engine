#pragma once
#include "../../lua/test/LuaValueTestTypes.hpp"
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <optional>
#include <vector>
namespace lux::simulation::na1
{
inline std::optional<script::CppScriptEventSource<std::int32_t>> event_source;
inline std::size_t constructed{}, destroyed{}, started{}, completed{}, frames_destroyed{}, unreachable{};
inline std::uint32_t mode{};
inline std::vector<std::int32_t> results;
struct FrameLifetime final
{
    ~FrameLifetime()
    {
        ++frames_destroyed;
    }
};
struct LUX_TYPE_INFO(compile_time) NativeTask final
{
    inline static constexpr std::array SyncStepShapes{script::scriptSyncStepShape<std::int32_t(std::int32_t)>(),
                                                      script::scriptSyncStepShape<void(std::int32_t)>()};
    NativeTask() noexcept : index(constructed++)
    {
    }
    ~NativeTask()
    {
        ++destroyed;
    }
    LUX_METHOD(script_export = "task.run", script_coroutine = true)
    script::ScriptCoroutine run(script::ScriptCoroutineContext &context) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        if (mode == 1U)
        {
            co_await context.fail({0U, script::EScriptSyncStepError::BACKEND_FAILURE, -771});
            ++unreachable;
        }
        auto bound = context.bindStep<std::int32_t(std::int32_t)>(0U);
        if (!bound) co_await context.fail(bound.error());
        const auto count = mode == 3U ? 32U : 1U;
        for (std::uint32_t i{}; i < count; ++i)
        {
            const auto payload = co_await context.wait(*event_source);
            if (mode == 2U)
            {
                co_await context.fail({0U, script::EScriptSyncStepError::BACKEND_FAILURE, -772});
                ++unreachable;
            }
            const auto value = (*bound)(payload);
            if (!value)
            {
                co_await context.fail(value.error());
                ++unreachable;
            }
            results[index] = *value;
        }
        ++completed;
    }
    LUX_METHOD(script_export = "task.next", script_coroutine = true)
    script::ScriptCoroutine next(script::ScriptCoroutineContext &context) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        {
            auto value = context.callStep<std::int32_t(std::int32_t)>(0U, 1);
            if (!value)
                co_await context.fail(value.error());
            results[index] = *value;
        }
        co_await context.delay().nextStep();
        auto value = context.callStep<std::int32_t(std::int32_t)>(0U, 10);
        if (!value)
            co_await context.fail(value.error());
        results[index] = *value;
        ++completed;
    }
    LUX_METHOD(script_export = "task.sequence", script_coroutine = true)
    script::ScriptCoroutine sequence(script::ScriptCoroutineContext &context) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        {
            auto value = context.callStep<std::int32_t(std::int32_t)>(0U, 1);
            if (!value)
                co_await context.fail(value.error());
            results[index] = *value;
        }
        co_await context.delay().nextStep();
        const auto payload = co_await context.wait(*event_source);
        co_await context.delay().simulationSeconds(0.001);
        auto value = context.callStep<std::int32_t(std::int32_t)>(0U, payload);
        if (!value)
            co_await context.fail(value.error());
        results[index] = *value;
        ++completed;
    }
    LUX_METHOD(script_export = "task.boundary", script_coroutine = true)
    script::ScriptCoroutine boundary(script::ScriptCoroutineContext &context) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        auto rejected = mode == 6U ? context.callStep<std::int32_t(std::int32_t)>(999U, 1)
                                   : context.callStep<std::int32_t(double)>(0U, 1.5);
        if (!rejected)
            co_await context.fail(rejected.error());
        ++unreachable;
    }
    LUX_METHOD(script_export = "task.void", script_coroutine = true)
    script::ScriptCoroutine voidStep(script::ScriptCoroutineContext &context) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        const auto payload = co_await context.wait(*event_source);
        auto applied = context.callStep<void(std::int32_t)>(1U, payload);
        if (!applied)
            co_await context.fail(applied.error());
        ++completed;
    }
    LUX_METHOD(script_export = "task.other", script_coroutine = true)
    script::ScriptCoroutine other(script::ScriptCoroutineContext &context) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        const auto payload = co_await context.wait(*event_source);
        const auto result = context.callStep<std::int32_t(std::int32_t)>(0U, payload + 100);
        if (!result)
            co_await context.fail(result.error());
        results[index] = *result;
        ++completed;
    }
    LUX_METHOD(script_export = "task.large", script_coroutine = true)
    script::ScriptCoroutine large(script::ScriptCoroutineContext &context) noexcept
    {
        // Deliberately observable storage tests the existing hard limit, not an
        // average-size budget.
        volatile script::test::ValuePose records[16]{};
        FrameLifetime lifetime;
        ++started;
        for (std::int32_t i{}; i < 16; ++i)
            records[i].id = i + results[index];
        const auto payload = co_await context.wait(*event_source);
        results[index] = records[payload & 15].id;
        ++completed;
    }
    std::size_t index{};
};
struct LUX_TYPE_INFO(compile_time) NativePoseTask final
{
    inline static constexpr std::array SyncStepShapes{
        script::scriptSyncStepShape<std::int32_t(const script::test::ValuePose&)>(),
        script::scriptSyncStepShape<void(std::int32_t)>()};
    NativePoseTask() noexcept : index(constructed++)
    {
    }
    ~NativePoseTask()
    {
        ++destroyed;
    }
    LUX_METHOD(script_export = "task.run", script_coroutine = true)
    script::ScriptCoroutine run(script::ScriptCoroutineContext &context, const script::test::ValuePose &input) noexcept
    {
        FrameLifetime lifetime;
        ++started;
        {
            auto before = context.callStep<std::int32_t(const script::test::ValuePose &)>(0U, input);
            if (!before)
                co_await context.fail(before.error());
            results[index] = *before;
        }
        const auto payload = co_await context.wait(*event_source);
        auto after = context.callStep<std::int32_t(const script::test::ValuePose &)>(0U, input);
        if (!after)
            co_await context.fail(after.error());
        results[index] = *after + payload;
        ++completed;
    }
    std::size_t index{};
};
} // namespace lux::simulation::na1
