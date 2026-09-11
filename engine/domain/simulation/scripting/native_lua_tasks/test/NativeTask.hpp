#pragma once
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
        if (mode == 4U || mode == 5U)
        {
            const auto before = context.callStep<std::int32_t(std::int32_t)>(0U, 1);
            if (!before)
                co_await context.fail(before.error());
            results[index] = *before;
            co_await context.delay().nextStep();
        }
        const auto count = mode == 3U ? 32U : 1U;
        for (std::uint32_t i{}; i < count; ++i)
        {
            const auto payload = mode == 4U ? 10 : co_await context.wait(*event_source);
            if (mode == 5U)
                co_await context.delay().simulationSeconds(0.001);
            if (mode == 2U)
            {
                co_await context.fail({0U, script::EScriptSyncStepError::BACKEND_FAILURE, -772});
                ++unreachable;
            }
            const auto value = context.callStep<std::int32_t(std::int32_t)>(0U, payload);
            if (!value)
            {
                co_await context.fail(value.error());
                ++unreachable;
            }
            results[index] = *value;
        }
        ++completed;
    }
    std::size_t index{};
};
} // namespace lux::simulation::na1
