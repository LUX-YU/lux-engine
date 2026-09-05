#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/physics2d/abilities/PhysicsQuery2D.ability.generated.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>

#include <cstdint>
#include <optional>

namespace installed_consumer
{
    inline std::int32_t observed{};
    inline std::optional<lux::simulation::script::CppScriptEventSource<std::int32_t>> pulse_event;

    class LUX_TYPE_INFO(compile_time) CoroutineBehavior final
    {
    public:
        LUX_METHOD(script_export="consumer.run", script_coroutine=true)
        lux::simulation::script::ScriptCoroutine run(
            lux::simulation::script::ScriptCoroutineContext& context
        ) noexcept
        {
            auto physics = context.ability<lux::physics2d::PhysicsQuery2D>();
            if (!physics)
            {
                observed = -1;
                co_return;
            }
            const auto overlaps = physics->overlapsBox(0.0, 0.0, 0.25, 0.25);
            if (!overlaps || !*overlaps || !pulse_event)
            {
                observed = -2;
                co_return;
            }
            observed = 1;
            observed += co_await context.wait(*pulse_event);
            co_await context.delay().nextStep();
            observed += 100;
        }
    };
}
