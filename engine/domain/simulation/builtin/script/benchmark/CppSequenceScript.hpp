#pragma once

#include "ScriptBenchmarkAbility.hpp"
#include "ScriptBenchmarkAbility.ability.generated.hpp"
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <optional>

namespace lux::simulation::benchmark
{
    inline std::optional<script::CppScriptEventSource<std::int32_t>> sequence_event;
    inline std::optional<script::CppScriptEventSource<std::int32_t>> event_only_source;
    inline std::uint64_t event_only_checksum{};

    struct LUX_TYPE_INFO(compile_time) CppEventWaitScript final
    {
        LUX_METHOD(script_export = "lifecycle.tick", script_coroutine = true)
        script::ScriptCoroutine waitHook(script::ScriptCoroutineContext& context) noexcept
        {
            if (!event_only_source) co_return;
            const auto payload = co_await context.wait(*event_only_source);
            value += payload;
            event_only_checksum += static_cast<std::uint32_t>(payload);
        }

        LUX_METHOD(script_export = "event.callback", script_coroutine = true)
        script::ScriptCoroutine
        waitCallback(script::ScriptCoroutineContext& context, const std::int32_t& marker) noexcept
        {
            if (!event_only_source) co_return;
            const auto payload = co_await context.wait(*event_only_source);
            value += payload + marker;
            event_only_checksum += static_cast<std::uint32_t>(payload + marker);
        }

        std::uint64_t value{};
    };

    struct LUX_TYPE_INFO(compile_time) CppSequenceScript final
    {
        LUX_METHOD(script_export = "lifecycle.tick", script_coroutine = true)
        script::ScriptCoroutine run(script::ScriptCoroutineContext& context) noexcept
        {
            auto values = context.ability<script::benchmark::ValueAbility>();
            const bool missing_requirement = !values || !sequence_event;
            if (missing_requirement) co_return;
            ++value;
            co_await context.delay().nextStep();
            const auto payload = co_await context.wait(*sequence_event);
            co_await context.delay().simulationSeconds(0.001);
            values->write(value + payload);
        }

        std::int32_t value{1};
    };
}
