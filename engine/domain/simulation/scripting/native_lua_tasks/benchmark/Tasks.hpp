#pragma once
#include "../../lua/test/LuaValueTestTypes.hpp"
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <optional>
namespace lux::simulation::na1::cost
{
using namespace script;
inline std::optional<CppScriptEventSource<std::int32_t>> source;
struct LUX_TYPE_INFO(compile_time) Tasks final
{
    template <class Signature, class... Args>
    static ScriptSyncStepError step(ScriptCoroutineContext &c, std::uint32_t ordinal, Args &&...args) noexcept
    {
        auto r = c.callStep<Signature>(ordinal, std::forward<Args>(args)...);
        return r ? ScriptSyncStepError{ordinal, EScriptSyncStepError::BACKEND_FAILURE, 0} : r.error();
    }
    LUX_METHOD(script_export = "na1.event", script_coroutine = true)
    ScriptCoroutine event(ScriptCoroutineContext &c) noexcept
    {
        const auto payload = co_await c.wait(*source);
        auto error = step<void(std::int32_t)>(c, 0U, payload);
        if (error.status)
            co_await c.fail(error);
    }
    LUX_METHOD(script_export = "na1.next", script_coroutine = true)
    ScriptCoroutine next(ScriptCoroutineContext &c) noexcept
    {
        auto error = step<void()>(c, 1U);
        if (error.status)
            co_await c.fail(error);
        co_await c.delay().nextStep();
        error = step<void()>(c, 2U);
        if (error.status)
            co_await c.fail(error);
    }
    LUX_METHOD(script_export = "na1.sequence", script_coroutine = true)
    ScriptCoroutine sequence(ScriptCoroutineContext &c) noexcept
    {
        auto error = step<void()>(c, 1U);
        if (error.status)
            co_await c.fail(error);
        co_await c.delay().nextStep();
        const auto payload = co_await c.wait(*source);
        co_await c.delay().simulationSeconds(0.001);
        error = step<void(std::int32_t)>(c, 3U, payload);
        if (error.status)
            co_await c.fail(error);
    }
    LUX_METHOD(script_export = "na1.long", script_coroutine = true)
    ScriptCoroutine longTask(ScriptCoroutineContext &c) noexcept
    {
        for (std::uint32_t i{}; i < 32U; ++i)
        {
            const auto payload = co_await c.wait(*source);
            auto error = step<void(std::int32_t)>(c, 0U, payload);
            if (error.status)
                co_await c.fail(error);
        }
    }
    LUX_METHOD(script_export = "na1.pose", script_coroutine = true)
    ScriptCoroutine pose(ScriptCoroutineContext &c, const script::test::ValuePose &input) noexcept
    {
        auto error = step<void(const script::test::ValuePose &)>(c, 4U, input);
        if (error.status)
            co_await c.fail(error);
        const auto payload = co_await c.wait(*source);
        error = step<void(const script::test::ValuePose &, std::int32_t)>(c, 5U, input, payload);
        if (error.status)
            co_await c.fail(error);
    }
};
} // namespace lux::simulation::na1::cost
