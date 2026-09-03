#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/function/script/ScriptAbilityAsync.hpp>

#include <memory>
#include <new>
#include <utility>

namespace lux::script
{
    namespace detail
    {
        template <class Result>
        struct ErasedCompletionAdapter final
        {
            explicit ErasedCompletionAdapter(ScriptAbilityErasedCompletion completion_value) noexcept
                : completion(std::move(completion_value))
            {
            }

            [[nodiscard]] static lux::cxx::expected<void, EScriptAbilityCompletionError> success(
                void* state,
                Result value
            ) noexcept
            {
                return static_cast<ErasedCompletionAdapter*>(state)->completion.success(
                    lux::semantic::typeId(lux::semantic::TypeTraits<Result>::CanonicalName),
                    std::addressof(value),
                    sizeof(Result)
                );
            }

            [[nodiscard]] static lux::cxx::expected<void, EScriptAbilityCompletionError> failure(
                void* state,
                ScriptAbilityOperationError error
            ) noexcept
            {
                return static_cast<ErasedCompletionAdapter*>(state)->completion.fail(error);
            }

            [[nodiscard]] static bool active(void* state) noexcept
            {
                return static_cast<ErasedCompletionAdapter*>(state)->completion.active();
            }

            ScriptAbilityErasedCompletion completion;
        };

        template <>
        struct ErasedCompletionAdapter<void> final
        {
            explicit ErasedCompletionAdapter(ScriptAbilityErasedCompletion completion_value) noexcept
                : completion(std::move(completion_value))
            {
            }

            [[nodiscard]] static lux::cxx::expected<void, EScriptAbilityCompletionError> success(
                void* state
            ) noexcept
            {
                return static_cast<ErasedCompletionAdapter*>(state)->completion.success();
            }

            [[nodiscard]] static lux::cxx::expected<void, EScriptAbilityCompletionError> failure(
                void* state,
                ScriptAbilityOperationError error
            ) noexcept
            {
                return static_cast<ErasedCompletionAdapter*>(state)->completion.fail(error);
            }

            [[nodiscard]] static bool active(void* state) noexcept
            {
                return static_cast<ErasedCompletionAdapter*>(state)->completion.active();
            }

            ScriptAbilityErasedCompletion completion;
        };
    } // namespace detail

    template <class Result>
    [[nodiscard]] lux::cxx::expected<ScriptAbilityCompletion<Result>, ScriptAbilityOperationError>
    adaptScriptAbilityCompletion(ScriptAbilityErasedCompletion completion) noexcept
    {
        using Adapter = detail::ErasedCompletionAdapter<Result>;
        std::shared_ptr<Adapter> adapter;
        try
        {
            adapter = std::make_shared<Adapter>(std::move(completion));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected<ScriptAbilityOperationError>(ScriptAbilityOperationError{
                static_cast<std::int32_t>(EScriptAbilityErasedCallStatus::COMPLETION_ALLOCATION_FAILURE)
            });
        }
        return ScriptAbilityCompletion<Result>::bind(
            std::static_pointer_cast<void>(adapter),
            &Adapter::success,
            &Adapter::failure,
            &Adapter::active
        );
    }
} // namespace lux::script
