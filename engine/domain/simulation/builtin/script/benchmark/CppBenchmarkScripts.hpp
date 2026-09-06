#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
namespace lux::simulation::benchmark
{
using namespace script;
inline std::uint64_t cpp_coroutine_checksum{};
inline std::uint64_t cpp_update_checksum{};
struct LUX_TYPE_INFO(compile_time) CppUpdateObject final
{
    std::uint64_t value{};
    LUX_METHOD(script_export = "lifecycle.begin", script_lifecycle = begin_play)
    void begin() noexcept { value = 0U; }
    LUX_METHOD(script_export = "lifecycle.tick")
    void tick() noexcept { ++value; }
    LUX_METHOD(script_export = "lifecycle.end", script_lifecycle = end_play)
    void end(EScriptEndPlayReason) noexcept { cpp_update_checksum += value; }
};
inline std::uint32_t region_reaction{};
struct LUX_TYPE_INFO(compile_time) RegionScript final
{
    std::uint32_t history{};
    LUX_METHOD(script_export = "region.first")
    void first(std::uint32_t sample) noexcept
    {
        history = history * 1664525U + sample + 1013904223U;
        region_reaction = history;
    }
    LUX_METHOD(script_export = "region.second")
    void second(std::uint32_t sample) noexcept
    {
        history ^= sample + (history << 5U) + (history >> 3U);
        region_reaction = history;
    }
};
struct LUX_TYPE_INFO(compile_time) CppLifecycleObject final
{
    std::uint64_t value{};
    LUX_METHOD(script_export = "lifecycle.begin", script_lifecycle = begin_play)
    void begin() noexcept
    {
        value = 10U;
    }
    LUX_METHOD(script_export = "lifecycle.tick")
    void tick() noexcept
    {
        ++value;
    }
    LUX_METHOD(script_export = "lifecycle.end", script_lifecycle = end_play)
    void end(EScriptEndPlayReason reason) noexcept
    {
        if (reason != EScriptEndPlayReason::RUNTIME_STOPPED || value != 11U)
            std::terminate();
    }
};
struct LUX_TYPE_INFO(compile_time) CppCoroutineBenchmarkObject final
{
    LUX_METHOD(script_export = "coroutine.run", script_coroutine = true)
    ScriptCoroutine run(ScriptCoroutineContext &context) noexcept
    {
        co_await CppStaticCoroutineAccess::makeAwaiter<void>(
            context, [](ScriptCoroutineContext &, ScriptStepContext &step) noexcept {
                const auto waiting = step.awaitables.create();
                return waiting ? ScriptStepResult::suspended(waiting->id) : ScriptStepResult::failed(-1);
            });
        ++cpp_coroutine_checksum;
    }
};
} // namespace lux::simulation::benchmark
