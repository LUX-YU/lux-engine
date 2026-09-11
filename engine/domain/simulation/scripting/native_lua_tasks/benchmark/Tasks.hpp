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
    LUX_METHOD(script_export = "na1.event", script_coroutine = true)
    ScriptCoroutine event(ScriptCoroutineContext &c) noexcept
    {
        const auto payload = co_await c.wait(*source);
        auto r = c.callStep<void(std::int32_t)>(0U, payload);
        if (!r)
            co_await c.fail(r.error());
    }
    LUX_METHOD(script_export = "na1.next", script_coroutine = true)
    ScriptCoroutine next(ScriptCoroutineContext &c) noexcept
    {
        {
            auto r = c.callStep<void()>(1U);
            if (!r)
                co_await c.fail(r.error());
        }
        co_await c.delay().nextStep();
        auto r = c.callStep<void()>(2U);
        if (!r)
            co_await c.fail(r.error());
    }
    LUX_METHOD(script_export = "na1.sequence", script_coroutine = true)
    ScriptCoroutine sequence(ScriptCoroutineContext &c) noexcept
    {
        {
            auto r = c.callStep<void()>(1U);
            if (!r)
                co_await c.fail(r.error());
        }
        co_await c.delay().nextStep();
        const auto payload = co_await c.wait(*source);
        co_await c.delay().simulationSeconds(0.001);
        auto r = c.callStep<void(std::int32_t)>(3U, payload);
        if (!r)
            co_await c.fail(r.error());
    }
    LUX_METHOD(script_export = "na1.long", script_coroutine = true)
    ScriptCoroutine longTask(ScriptCoroutineContext &c) noexcept
    {
        for (std::uint32_t i{}; i < 32U; ++i)
        {
            const auto payload = co_await c.wait(*source);
            auto r = c.callStep<void(std::int32_t)>(0U, payload);
            if (!r)
                co_await c.fail(r.error());
        }
    }
    LUX_METHOD(script_export = "na1.pose", script_coroutine = true)
    ScriptCoroutine pose(ScriptCoroutineContext &c, const script::test::ValuePose &input) noexcept
    {
        {
            auto r = c.callStep<void(const script::test::ValuePose &)>(4U, input);
            if (!r)
                co_await c.fail(r.error());
        }
        const auto payload = co_await c.wait(*source);
        auto r = c.callStep<void(const script::test::ValuePose &, std::int32_t)>(5U, input, payload);
        if (!r)
            co_await c.fail(r.error());
    }
};
} // namespace lux::simulation::na1::cost
