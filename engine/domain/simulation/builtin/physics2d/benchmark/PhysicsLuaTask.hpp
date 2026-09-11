#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <optional>
namespace lux::physics2d::benchmark
{
using namespace lux::simulation::script;
inline std::optional<CppScriptEventSource<std::int32_t>> lua_task_event;
struct LUX_TYPE_INFO(compile_time) PhysicsLuaTask final
{
    LUX_METHOD(script_export = "physics.lua.task", script_coroutine = true)
    ScriptCoroutine run(ScriptCoroutineContext &context) noexcept
    {
        {
            auto result = context.callStep<void()>(0U);
            if (!result)
                co_await context.fail(result.error());
        }
        const auto payload = co_await context.wait(*lua_task_event);
        {
            auto result = context.callStep<void(std::int32_t)>(1U, payload);
            if (!result)
                co_await context.fail(result.error());
        }
        co_await context.delay().nextStep();
        auto result = context.callStep<void()>(2U);
        if (!result)
            co_await context.fail(result.error());
    }
};
} // namespace lux::physics2d::benchmark
