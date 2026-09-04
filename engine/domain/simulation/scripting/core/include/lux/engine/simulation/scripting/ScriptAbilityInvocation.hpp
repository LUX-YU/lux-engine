#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

#include <functional>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace lux::simulation::script
{
    enum class EScriptAbilityInvocationStatus : std::int32_t
    {
        INVALID_CONTEXT = -1,
        AWAITABLE_CAPACITY_EXCEEDED = -2,
        AWAITABLE_ALLOCATION_FAILURE = -3,
        STOPPING = -4,
        INVALID_START_RESULT = -5,
        RESULT_NOT_TRANSPORTABLE = -6,
    };

    namespace detail
    {
        [[nodiscard]] constexpr std::int32_t invocationStatus(EScriptAbilityInvocationStatus status) noexcept
        {
            return static_cast<std::int32_t>(status);
        }

        [[nodiscard]] constexpr std::int32_t awaitableCreateStatus(EScriptAwaitableCreateError error) noexcept
        {
            switch (error)
            {
            case EScriptAwaitableCreateError::CAPACITY_EXCEEDED:
                return invocationStatus(EScriptAbilityInvocationStatus::AWAITABLE_CAPACITY_EXCEEDED);
            case EScriptAwaitableCreateError::ALLOCATION_FAILURE:
                return invocationStatus(EScriptAbilityInvocationStatus::AWAITABLE_ALLOCATION_FAILURE);
            case EScriptAwaitableCreateError::STOPPING:
                return invocationStatus(EScriptAbilityInvocationStatus::STOPPING);
            case EScriptAwaitableCreateError::EXTERNAL_RESULT_NOT_TRANSPORTABLE:
                return invocationStatus(EScriptAbilityInvocationStatus::RESULT_NOT_TRANSPORTABLE);
            case EScriptAwaitableCreateError::INVALID_INSTANCE:
            case EScriptAwaitableCreateError::INVALID_RESULT_TYPE:
                return invocationStatus(EScriptAbilityInvocationStatus::INVALID_CONTEXT);
            }
            return invocationStatus(EScriptAbilityInvocationStatus::INVALID_CONTEXT);
        }
    }

    template <class Result, class Starter>
        requires std::is_void_v<Result> ||
            (lux::semantic::TypeDeclared<Result> && std::is_trivially_copyable_v<Result>)
    [[nodiscard]] ScriptStepResult invokeScriptAbilityAsync(
        ScriptStepContext& context,
        Starter&& starter
    ) noexcept
    {
        using Completion = lux::script::ScriptAbilityCompletion<Result>;
        static_assert(std::is_nothrow_invocable_r_v<lux::script::ScriptAbilityStartResult, Starter, Completion>);

        std::optional<lux::rdesc::ScriptValueType> result_type;
        if constexpr (!std::is_void_v<Result>)
        {
            try
            {
                result_type = lux::rdesc::makeScriptValueType<Result>();
            }
            catch (const std::bad_alloc&)
            {
                return ScriptStepResult::failed(
                    detail::invocationStatus(EScriptAbilityInvocationStatus::AWAITABLE_ALLOCATION_FAILURE)
                );
            }
        }

        auto awaiting = context.awaitables.create(std::move(result_type));
        if (!awaiting)
            return ScriptStepResult::failed(detail::awaitableCreateStatus(awaiting.error()));

        auto completion = Completion::fromErased(std::move(awaiting->completion).intoAbilityCompletion());
        auto started = std::invoke(std::forward<Starter>(starter), std::move(completion));
        if (started)
            return ScriptStepResult::suspended(awaiting->id);

        context.awaitables.discard(awaiting->id);
        if (!started.error().valid())
        {
            return ScriptStepResult::failed(
                detail::invocationStatus(EScriptAbilityInvocationStatus::INVALID_START_RESULT)
            );
        }
        return ScriptStepResult::failed(started.error().status);
    }

    [[nodiscard]] inline ScriptStepResult invokeScriptAbilityAsyncErased(
        ScriptStepContext& context,
        void* provider_context,
        const void* typed_dispatch,
        lux::script::ScriptAbilityErasedStartFn start,
        std::span<const lux::script::ScriptAbilityInputSlot> arguments,
        const lux::script::ScriptAbilityValueDescription* result
    ) noexcept
    {
        if (typed_dispatch == nullptr || start == nullptr)
            return ScriptStepResult::failed(detail::invocationStatus(EScriptAbilityInvocationStatus::INVALID_CONTEXT));

        std::optional<lux::rdesc::ScriptValueType> result_type;
        if (result != nullptr)
        {
            try
            {
                result_type = lux::rdesc::ScriptValueType{
                    std::string(result->canonical_name),
                    result->type_id,
                    lux::semantic::EValuePass::VALUE,
                    result->abi_kind,
                    result->size,
                    result->alignment
                };
            }
            catch (const std::bad_alloc&)
            {
                return ScriptStepResult::failed(
                    detail::invocationStatus(EScriptAbilityInvocationStatus::AWAITABLE_ALLOCATION_FAILURE)
                );
            }
        }

        auto awaiting = context.awaitables.create(std::move(result_type));
        if (!awaiting)
            return ScriptStepResult::failed(detail::awaitableCreateStatus(awaiting.error()));

        auto completion = std::move(awaiting->completion).intoAbilityCompletion();
        auto started = start(provider_context, typed_dispatch, arguments, std::move(completion));
        if (started)
            return ScriptStepResult::suspended(awaiting->id);

        context.awaitables.discard(awaiting->id);
        if (!started.error().valid())
        {
            return ScriptStepResult::failed(
                detail::invocationStatus(EScriptAbilityInvocationStatus::INVALID_START_RESULT)
            );
        }
        return ScriptStepResult::failed(started.error().status);
    }
} // namespace lux::simulation::script
