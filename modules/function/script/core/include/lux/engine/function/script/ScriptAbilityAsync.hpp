#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::script
{
    template <class Result> class TScriptAbilityCompletion final
    {
    public:
        static_assert(!std::is_reference_v<Result>);
        static_assert(!std::is_volatile_v<Result>);
        static_assert(std::is_nothrow_move_constructible_v<Result>);

        using CompletionResult = lux::cxx::expected<void, EScriptAbilityCompletionError>;

        TScriptAbilityCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(completion_);
        }

        [[nodiscard]] CompletionResult success(Result value) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected<EScriptAbilityCompletionError>(EScriptAbilityCompletionError::STALE);
            return completion_.success(
                lux::semantic::typeId(lux::semantic::TTypeTraits<Result>::CanonicalName),
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

        [[nodiscard]] static TScriptAbilityCompletion fromErased(ScriptAbilityErasedCompletion completion) noexcept
        {
            return TScriptAbilityCompletion(std::move(completion));
        }

    private:
        friend struct detail::ScriptAbilityOwnerCompletionAccess;

        explicit TScriptAbilityCompletion(ScriptAbilityErasedCompletion completion) noexcept
            : completion_(std::move(completion))
        {}

        ScriptAbilityErasedCompletion completion_;
    };

    template <> class TScriptAbilityCompletion<void> final
    {
    public:
        using CompletionResult = lux::cxx::expected<void, EScriptAbilityCompletionError>;

        TScriptAbilityCompletion() = default;

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

        [[nodiscard]] static TScriptAbilityCompletion fromErased(ScriptAbilityErasedCompletion completion) noexcept
        {
            return TScriptAbilityCompletion(std::move(completion));
        }

    private:
        friend struct detail::ScriptAbilityOwnerCompletionAccess;

        explicit TScriptAbilityCompletion(ScriptAbilityErasedCompletion completion) noexcept
            : completion_(std::move(completion))
        {}

        ScriptAbilityErasedCompletion completion_;
    };

    template <class Ability> class TScriptAbilityStarter;

    namespace detail
    {
        struct ScriptAbilityOwnerCompletionAccess final
        {
            // Owner-only source association. A caller must match its own completion context;
            // this returns value tokens, never the owner pointer or mutable completion authority.
            [[nodiscard]] static bool matchOwner(
                const TScriptAbilityCompletion<void>& completion,
                const void* owner,
                std::uint64_t& token_a,
                std::uint64_t& token_b
            ) noexcept
            {
                const auto& erased = completion.completion_;
                const bool matches = owner != nullptr && erased.owner_context_ == owner &&
                                     erased.owner_success_ != nullptr && erased.owner_failure_ != nullptr;
                token_a = matches ? erased.token_a_ : 0U;
                token_b = matches ? erased.token_b_ : 0U;
                return matches;
            }

            template <class Result>
            [[nodiscard]] static typename TScriptAbilityCompletion<Result>::CompletionResult success(
                const TScriptAbilityCompletion<Result>& completion,
                Result value
            ) noexcept
            {
                return completion.completion_.successOwner(
                    lux::semantic::typeId(lux::semantic::TTypeTraits<Result>::CanonicalName),
                    std::addressof(value),
                    sizeof(Result)
                );
            }

            [[nodiscard]] static TScriptAbilityCompletion<void>::CompletionResult success(
                const TScriptAbilityCompletion<void>& completion
            ) noexcept
            {
                return completion.completion_.successOwner(lux::semantic::InvalidTypeId, nullptr, 0U);
            }

            template <class Result>
            [[nodiscard]] static typename TScriptAbilityCompletion<Result>::CompletionResult fail(
                const TScriptAbilityCompletion<Result>& completion,
                ScriptAbilityOperationError error
            ) noexcept
            {
                return completion.completion_.failOwner(error);
            }
        };
    } // namespace detail
} // namespace lux::script
