#pragma once

#include "ScriptBenchmarkAbility.hpp"
#include "ScriptBenchmarkAbility.ability.generated.hpp"
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <optional>

namespace lux::simulation::benchmark
{
    inline std::optional<script::CppScriptEventSource<std::int32_t>> sequence_event;

    struct LUX_TYPE_INFO(compile_time) CppSequenceScript final
    {
        LUX_METHOD(script_export = "lifecycle.tick", script_coroutine = true)
        script::ScriptCoroutine run(script::ScriptCoroutineContext& context) noexcept
        {
            auto values = context.ability<script::benchmark::ValueAbility>();
            if (!values || !sequence_event) co_return;
            ++value;
            co_await context.delay().nextStep();
            const auto payload = co_await context.wait(*sequence_event);
            co_await context.delay().simulationSeconds(0.001);
            values->write(value + payload);
        }

        std::int32_t value{1};
    };
}
