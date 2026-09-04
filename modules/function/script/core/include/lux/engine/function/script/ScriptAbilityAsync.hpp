#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::script
{
    template <class Result>
    class ScriptAbilityCompletion final
    {
    public:
        static_assert(!std::is_reference_v<Result>);
        static_assert(!std::is_volatile_v<Result>);
        static_assert(std::is_nothrow_move_constructible_v<Result>);

        using CompletionResult = lux::cxx::expected<void, EScriptAbilityCompletionError>;

        ScriptAbilityCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(completion_);
        }

        [[nodiscard]] CompletionResult success(Result value) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected<EScriptAbilityCompletionError>(EScriptAbilityCompletionError::STALE);
            return completion_.success(
                lux::semantic::typeId(lux::semantic::TypeTraits<Result>::CanonicalName),
                std::addressof(value),
                sizeof(Result)
            );
        }

        [[nodiscard]] CompletionResult fail(ScriptAbilityOperationError error) const noexcept
        {
            return completion_.fail(error);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return completion_.active();
        }

        [[nodiscard]] static ScriptAbilityCompletion fromErased(
            ScriptAbilityErasedCompletion completion
        ) noexcept
        {
            return ScriptAbilityCompletion(std::move(completion));
        }

    private:
        explicit ScriptAbilityCompletion(ScriptAbilityErasedCompletion completion) noexcept
            : completion_(std::move(completion))
        {
        }

        ScriptAbilityErasedCompletion completion_;
    };

    template <>
    class ScriptAbilityCompletion<void> final
    {
    public:
        using CompletionResult = lux::cxx::expected<void, EScriptAbilityCompletionError>;

        ScriptAbilityCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(completion_);
        }

        [[nodiscard]] CompletionResult success() const noexcept
        {
            return completion_.success();
        }

        [[nodiscard]] CompletionResult fail(ScriptAbilityOperationError error) const noexcept
        {
            return completion_.fail(error);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return completion_.active();
        }

        [[nodiscard]] static ScriptAbilityCompletion fromErased(
            ScriptAbilityErasedCompletion completion
        ) noexcept
        {
            return ScriptAbilityCompletion(std::move(completion));
        }

    private:
        explicit ScriptAbilityCompletion(ScriptAbilityErasedCompletion completion) noexcept
            : completion_(std::move(completion))
        {
        }

        ScriptAbilityErasedCompletion completion_;
    };

    template <class Ability>
    class ScriptAbilityStarter;
} // namespace lux::script
