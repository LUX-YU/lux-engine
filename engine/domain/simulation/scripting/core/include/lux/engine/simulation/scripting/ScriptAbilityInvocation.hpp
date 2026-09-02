#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::simulation::script
{
    enum class EScriptAbilityInvocationStatus : std::int32_t
    {
        INVALID_CONTEXT = -1,
        AWAITABLE_CAPACITY_EXCEEDED = -2,
        AWAITABLE_ALLOCATION_FAILURE = -3,
        COMPLETION_ADAPTER_ALLOCATION_FAILURE = -4,
        STOPPING = -5,
        INVALID_START_RESULT = -6,
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
            case EScriptAwaitableCreateError::INVALID_INSTANCE:
            case EScriptAwaitableCreateError::INVALID_RESULT_TYPE:
                return invocationStatus(EScriptAbilityInvocationStatus::INVALID_CONTEXT);
            }
            return invocationStatus(EScriptAbilityInvocationStatus::INVALID_CONTEXT);
        }

        [[nodiscard]] constexpr lux::script::EScriptAbilityCompletionError mapCompletionError(
            EScriptAwaitableCompletionError error
        ) noexcept
        {
            switch (error)
            {
            case EScriptAwaitableCompletionError::INVALID_ID:
                return lux::script::EScriptAbilityCompletionError::STALE;
            case EScriptAwaitableCompletionError::INVALID_VALUE:
                return lux::script::EScriptAbilityCompletionError::INVALID_VALUE;
            case EScriptAwaitableCompletionError::ALREADY_TERMINAL:
                return lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED;
            case EScriptAwaitableCompletionError::RESUME_QUEUE_FULL:
                return lux::script::EScriptAbilityCompletionError::BACKPRESSURE;
            case EScriptAwaitableCompletionError::STOPPING:
                return lux::script::EScriptAbilityCompletionError::STOPPING;
            }
            return lux::script::EScriptAbilityCompletionError::STALE;
        }

        template <class Result>
        struct AbilityCompletionAdapter final
        {
            explicit AbilityCompletionAdapter(ScriptAwaitableCompletion completion_value) noexcept
                : completion(std::move(completion_value))
            {
            }

            [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> success(
                void* state,
                Result value
            ) noexcept
            {
                ScriptOwnedResumeValue owned;
                try
                {
                    owned.type = lux::rdesc::makeScriptValueType<Result>();
                    owned.bytes.resize(sizeof(Result));
                }
                catch (const std::bad_alloc&)
                {
                    return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::ALLOCATION_FAILURE);
                }
                std::memcpy(owned.bytes.data(), std::addressof(value), sizeof(Result));
                auto completed = static_cast<AbilityCompletionAdapter*>(state)->completion.ready(std::move(owned));
                if (!completed)
                    return lux::cxx::unexpected(mapCompletionError(completed.error()));
                return {};
            }

            [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> failure(
                void* state,
                lux::script::ScriptAbilityOperationError error
            ) noexcept
            {
                auto completed = static_cast<AbilityCompletionAdapter*>(state)->completion.fail({error.status});
                if (!completed)
                    return lux::cxx::unexpected(mapCompletionError(completed.error()));
                return {};
            }

            ScriptAwaitableCompletion completion;
        };

        template <>
        struct AbilityCompletionAdapter<void> final
        {
            explicit AbilityCompletionAdapter(ScriptAwaitableCompletion completion_value) noexcept
                : completion(std::move(completion_value))
            {
            }

            [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> success(
                void* state
            ) noexcept
            {
                auto completed = static_cast<AbilityCompletionAdapter*>(state)->completion.ready();
                if (!completed)
                    return lux::cxx::unexpected(mapCompletionError(completed.error()));
                return {};
            }

            [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> failure(
                void* state,
                lux::script::ScriptAbilityOperationError error
            ) noexcept
            {
                auto completed = static_cast<AbilityCompletionAdapter*>(state)->completion.fail({error.status});
                if (!completed)
                    return lux::cxx::unexpected(mapCompletionError(completed.error()));
                return {};
            }

            ScriptAwaitableCompletion completion;
        };
    } // namespace detail

    template <class Result, class Starter>
        requires std::is_void_v<Result> ||
            (lux::semantic::TypeDeclared<Result> && std::is_trivially_copyable_v<Result>)
    [[nodiscard]] ScriptStepResult invokeScriptAbilityAsync(
        ScriptStepContext& context,
        Starter&& starter
    ) noexcept
    {
        using Completion = lux::script::ScriptAbilityCompletion<Result>;
        using Adapter = detail::AbilityCompletionAdapter<Result>;
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

        std::shared_ptr<Adapter> adapter;
        try
        {
            adapter = std::make_shared<Adapter>(std::move(awaiting->completion));
        }
        catch (const std::bad_alloc&)
        {
            context.awaitables.discard(awaiting->id);
            return ScriptStepResult::failed(
                detail::invocationStatus(EScriptAbilityInvocationStatus::COMPLETION_ADAPTER_ALLOCATION_FAILURE)
            );
        }

        auto completion = Completion::bind(
            std::static_pointer_cast<void>(adapter),
            &Adapter::success,
            &Adapter::failure
        );
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
} // namespace lux::simulation::script
