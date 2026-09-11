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
    inline static constexpr std::array SyncStepShapes{scriptSyncStepShape<void()>(),
                                                      scriptSyncStepShape<void(std::int32_t)>()};
    template <class Signature, class... Args>
    static auto step(ScriptCoroutineContext& context, std::uint32_t ordinal, Args&&... args) noexcept
    {
        return context.callStep<Signature>(ordinal, std::forward<Args>(args)...);
    }
    LUX_METHOD(script_export = "physics.lua.task", script_coroutine = true)
    ScriptCoroutine run(ScriptCoroutineContext &context) noexcept
    {
        auto error = step<void()>(context, 0U);
        if (!error)
            co_await context.fail(error.error());
        const auto payload = co_await context.wait(*lua_task_event);
        error = step<void(std::int32_t)>(context, 1U, payload);
        if (!error)
            co_await context.fail(error.error());
        co_await context.delay().nextStep();
        error = step<void()>(context, 2U);
        if (!error)
            co_await context.fail(error.error());
    }
};
} // namespace lux::physics2d::benchmark
