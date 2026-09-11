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
    // Synchronous expected/argument temporaries belong to an ordinary C++ stack, not to the coroutine frame.
    template <class Signature, class... Args>
    static ScriptSyncStepError step(ScriptCoroutineContext &context, std::uint32_t ordinal, Args &&...args) noexcept
    {
        auto result = context.callStep<Signature>(ordinal, std::forward<Args>(args)...);
        return result ? ScriptSyncStepError{ordinal, EScriptSyncStepError::BACKEND_FAILURE, 0} : result.error();
    }
    LUX_METHOD(script_export = "physics.lua.task", script_coroutine = true)
    ScriptCoroutine run(ScriptCoroutineContext &context) noexcept
    {
        auto error = step<void()>(context, 0U);
        if (error.status)
            co_await context.fail(error);
        const auto payload = co_await context.wait(*lua_task_event);
        error = step<void(std::int32_t)>(context, 1U, payload);
        if (error.status)
            co_await context.fail(error);
        co_await context.delay().nextStep();
        error = step<void()>(context, 2U);
        if (error.status)
            co_await context.fail(error);
    }
};
} // namespace lux::physics2d::benchmark
