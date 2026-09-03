#pragma once

#include <lux/engine/function/script/ScriptAbilityOperation.hpp>

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
        using SuccessFn = CompletionResult (*)(void*, Result) noexcept;
        using FailureFn = CompletionResult (*)(void*, ScriptAbilityOperationError) noexcept;
        using ActiveFn = bool (*)(void*) noexcept;

        ScriptAbilityCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return state_ != nullptr && success_ != nullptr && failure_ != nullptr;
        }

        [[nodiscard]] CompletionResult success(Result value) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected(EScriptAbilityCompletionError::STALE);
            return success_(state_.get(), std::move(value));
        }

        [[nodiscard]] CompletionResult fail(ScriptAbilityOperationError error) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected(EScriptAbilityCompletionError::STALE);
            if (!error.valid())
                return lux::cxx::unexpected(EScriptAbilityCompletionError::INVALID_VALUE);
            return failure_(state_.get(), error);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return *this && (active_ == nullptr || active_(state_.get()));
        }

        [[nodiscard]] static ScriptAbilityCompletion bind(
            std::shared_ptr<void> state,
            SuccessFn success,
            FailureFn failure,
            ActiveFn active = nullptr
        ) noexcept
        {
            return ScriptAbilityCompletion(std::move(state), success, failure, active);
        }

    private:
        ScriptAbilityCompletion(
            std::shared_ptr<void> state,
            SuccessFn success,
            FailureFn failure,
            ActiveFn active
        ) noexcept
            : state_(std::move(state)), success_(success), failure_(failure), active_(active)
        {
        }

        std::shared_ptr<void> state_;
        SuccessFn success_{};
        FailureFn failure_{};
        ActiveFn active_{};
    };

    template <>
    class ScriptAbilityCompletion<void> final
    {
    public:
        using CompletionResult = lux::cxx::expected<void, EScriptAbilityCompletionError>;
        using SuccessFn = CompletionResult (*)(void*) noexcept;
        using FailureFn = CompletionResult (*)(void*, ScriptAbilityOperationError) noexcept;
        using ActiveFn = bool (*)(void*) noexcept;

        ScriptAbilityCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return state_ != nullptr && success_ != nullptr && failure_ != nullptr;
        }

        [[nodiscard]] CompletionResult success() const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected(EScriptAbilityCompletionError::STALE);
            return success_(state_.get());
        }

        [[nodiscard]] CompletionResult fail(ScriptAbilityOperationError error) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected(EScriptAbilityCompletionError::STALE);
            if (!error.valid())
                return lux::cxx::unexpected(EScriptAbilityCompletionError::INVALID_VALUE);
            return failure_(state_.get(), error);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return *this && (active_ == nullptr || active_(state_.get()));
        }

        [[nodiscard]] static ScriptAbilityCompletion bind(
            std::shared_ptr<void> state,
            SuccessFn success,
            FailureFn failure,
            ActiveFn active = nullptr
        ) noexcept
        {
            return ScriptAbilityCompletion(std::move(state), success, failure, active);
        }

    private:
        ScriptAbilityCompletion(
            std::shared_ptr<void> state,
            SuccessFn success,
            FailureFn failure,
            ActiveFn active
        ) noexcept
            : state_(std::move(state)), success_(success), failure_(failure), active_(active)
        {
        }

        std::shared_ptr<void> state_;
        SuccessFn success_{};
        FailureFn failure_{};
        ActiveFn active_{};
    };

    template <class Ability>
    class ScriptAbilityStarter;
} // namespace lux::script
