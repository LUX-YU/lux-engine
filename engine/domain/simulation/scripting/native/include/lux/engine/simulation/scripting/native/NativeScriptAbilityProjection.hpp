#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/function/script/native/ScriptAbilityNative.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/native/visibility.h>

#include <type_traits>
#include <utility>

namespace lux::simulation::script::detail
{
    struct LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_PUBLIC NativeAbilityProjectionAccess final
    {
        [[nodiscard]] static ScriptStepContext* step(void* invocation) noexcept;
        static void beginAbility(void* invocation) noexcept;
    };

    template <class Result, class Start>
    [[nodiscard]] int startNativeAbility(
        void* invocation,
        lux_script_async_token* waiting_on,
        Start start
    ) noexcept
    {
        auto* step = NativeAbilityProjectionAccess::step(invocation);
        if (step == nullptr || waiting_on == nullptr)
            return static_cast<std::int32_t>(lux::script::EScriptAbilityErasedCallStatus::INVALID_ARGUMENTS);
        NativeAbilityProjectionAccess::beginAbility(invocation);
        const auto result = invokeScriptAbilityAsync<Result>(*step, std::move(start));
        if (result.state == EScriptStepState::SUSPENDED && result.valid())
        {
            waiting_on->slot = result.waiting_on.slot;
            waiting_on->generation = result.waiting_on.generation;
            return 0;
        }
        return result.state == EScriptStepState::FAILED && result.error.valid() ? result.error.status : -1;
    }
} // namespace lux::simulation::script::detail
