#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <optional>
namespace installed_consumer
{
inline std::int32_t observed{};
inline std::size_t starts{}, ends{}, objects{}, destroys{}, frames{};
inline std::optional<lux::simulation::script::CppScriptEventSource<std::int32_t>> pulse_event;
struct Frame final { ~Frame() { ++frames; } };
struct LUX_TYPE_INFO(compile_time) CoroutineBehavior final
{
    CoroutineBehavior() noexcept { ++objects; }
    ~CoroutineBehavior() { ++destroys; }
    LUX_METHOD(script_export = "consumer.run", script_coroutine = true)
    lux::simulation::script::ScriptCoroutine run(lux::simulation::script::ScriptCoroutineContext& context) noexcept
    {
        Frame lifetime;
        ++starts;
        const auto payload = co_await context.wait(*pulse_event);
        co_await context.delay().nextStep();
        auto result = context.callStep<std::int32_t(std::int32_t)>(0U, payload);
        if (!result) co_await context.fail(result.error());
        observed = *result;
        ++ends;
    }
};
}
