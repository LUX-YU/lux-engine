#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/function/script/ScriptApi.hpp>
#include <lux/engine/function/script/ScriptAbilityOperation.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::script
{
    struct ScriptAbilityValueDescription;
    struct ScriptAbilityParameterDescription;

    using ScriptAbilityInputSlot = lux_script_value_slot;
    using ScriptAbilityOutputSlot = lux_script_value_slot;

    enum class EScriptAbilityErasedCallStatus : std::int32_t
    {
        INVALID_ARGUMENTS = 1001,
        INVALID_RESULTS = 1002,
        COMPLETION_ALLOCATION_FAILURE = 1003,
    };

    class ScriptAbilityErasedCompletion final
    {
    public:
        using CompletionResult = lux::cxx::expected<void, EScriptAbilityCompletionError>;
        using SuccessFn = CompletionResult (*)(
            void*,
            std::uint64_t,
            std::uint64_t,
            lux::semantic::TypeId,
            const void*,
            std::uint32_t
        ) noexcept;
        using FailureFn = CompletionResult (*)(
            void*,
            std::uint64_t,
            std::uint64_t,
            ScriptAbilityOperationError
        ) noexcept;
        using ActiveFn = bool (*)(void*, std::uint64_t, std::uint64_t) noexcept;

        ScriptAbilityErasedCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return lease_ != nullptr && context_ != nullptr && success_ != nullptr && failure_ != nullptr;
        }

        [[nodiscard]] CompletionResult success(
            lux::semantic::TypeId type,
            const void* value,
            std::uint32_t size
        ) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected<EScriptAbilityCompletionError>(EScriptAbilityCompletionError::STALE);
            return success_(context_, token_a_, token_b_, type, value, size);
        }

        [[nodiscard]] CompletionResult success() const noexcept
        {
            return success(lux::semantic::InvalidTypeId, nullptr, 0U);
        }

        [[nodiscard]] CompletionResult fail(ScriptAbilityOperationError error) const noexcept
        {
            if (!*this)
                return lux::cxx::unexpected<EScriptAbilityCompletionError>(EScriptAbilityCompletionError::STALE);
            return failure_(context_, token_a_, token_b_, error);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return *this && (active_ == nullptr || active_(context_, token_a_, token_b_));
        }

        [[nodiscard]] static ScriptAbilityErasedCompletion bind(
            std::shared_ptr<void> lease,
            void* context,
            std::uint64_t token_a,
            std::uint64_t token_b,
            SuccessFn success,
            FailureFn failure,
            ActiveFn active = nullptr
        ) noexcept
        {
            return ScriptAbilityErasedCompletion(
                std::move(lease),
                context,
                token_a,
                token_b,
                success,
                failure,
                active
            );
        }

    private:
        ScriptAbilityErasedCompletion(
            std::shared_ptr<void> lease,
            void* context,
            std::uint64_t token_a,
            std::uint64_t token_b,
            SuccessFn success,
            FailureFn failure,
            ActiveFn active
        ) noexcept
            : lease_(std::move(lease)),
              context_(context),
              token_a_(token_a),
              token_b_(token_b),
              success_(success),
              failure_(failure),
              active_(active)
        {
        }

        std::shared_ptr<void> lease_;
        void* context_{};
        std::uint64_t token_a_{};
        std::uint64_t token_b_{};
        SuccessFn success_{};
        FailureFn failure_{};
        ActiveFn active_{};
    };

    using ScriptAbilityErasedCallResult = lux::cxx::expected<void, ScriptAbilityOperationError>;
    using ScriptAbilityErasedInvokeFn = ScriptAbilityErasedCallResult (*)(
        void*,
        const void*,
        std::span<const ScriptAbilityInputSlot>,
        std::span<ScriptAbilityOutputSlot>
    ) noexcept;
    using ScriptAbilityErasedStartFn = ScriptAbilityStartResult (*)(
        void*,
        const void*,
        std::span<const ScriptAbilityInputSlot>,
        ScriptAbilityErasedCompletion
    ) noexcept;

    struct ScriptAbilityErasedMethodBinding final
    {
        ScriptApiMethodIdView method;
        EScriptApiMethodKind kind{EScriptApiMethodKind::QUERY};
        std::span<const ScriptAbilityParameterDescription> parameters;
        std::span<const ScriptAbilityValueDescription> results;
        ScriptAbilityErasedInvokeFn invoke{};
        ScriptAbilityErasedStartFn start{};
    };

    template <class Value>
    [[nodiscard]] constexpr bool scriptAbilityInputMatches(
        const ScriptAbilityInputSlot& slot
    ) noexcept
    {
        using Type = std::remove_cvref_t<Value>;
        return slot.data != nullptr && slot.type_id == lux::semantic::typeId(
            lux::semantic::TypeTraits<Type>::CanonicalName
        ) && slot.size == sizeof(Type);
    }

    template <class Value>
    [[nodiscard]] constexpr bool scriptAbilityOutputMatches(
        const ScriptAbilityOutputSlot& slot
    ) noexcept
    {
        using Type = std::remove_cvref_t<Value>;
        return slot.data != nullptr && slot.type_id == lux::semantic::typeId(
            lux::semantic::TypeTraits<Type>::CanonicalName
        ) && slot.size == sizeof(Type);
    }

    enum class EScriptAbilityReceiverKind : std::uint8_t
    {
        NONE,
        PROVIDER_INSTANCE,
    };

    enum class EScriptAbilityValueLifetime : std::uint8_t
    {
        OWNED_VALUE,
        STABLE_ID,
        BORROWED_STEP,
        AWAITABLE,
    };

    struct ScriptAbilityValueDescription final
    {
        lux::semantic::TypeId type_id{};
        std::string_view canonical_name;
        lux::semantic::EValuePass pass{lux::semantic::EValuePass::VALUE};
        std::uint8_t abi_kind{};
        std::uint32_t size{};
        std::uint32_t alignment{};
        EScriptAbilityValueLifetime lifetime{EScriptAbilityValueLifetime::OWNED_VALUE};
    };

    struct ScriptAbilityParameterDescription final
    {
        std::string_view name;
        ScriptAbilityValueDescription value;
    };

    [[nodiscard]] constexpr bool scriptAbilityCodeNameValid(std::string_view value) noexcept
    {
        if (value.empty())
            return false;
        const auto is_alpha = [](char character) noexcept {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') || character == '_';
        };
        const auto is_digit = [](char character) noexcept {
            return character >= '0' && character <= '9';
        };
        if (!is_alpha(value.front()))
            return false;
        for (const char character : value.substr(1U))
        {
            if (!is_alpha(character) && !is_digit(character))
                return false;
        }
        return true;
    }

    struct ScriptAbilityMethodDescription final
    {
        ScriptApiMethodIdView id;
        std::string_view name;
        std::string_view display_name;
        EScriptApiMethodKind kind{EScriptApiMethodKind::QUERY};
        std::span<const ScriptAbilityParameterDescription> parameters;
        std::span<const ScriptAbilityValueDescription> results;
    };

    struct ScriptAbilityDescription final
    {
        ScriptApiContractIdView id;
        std::string_view name;
        std::string_view display_name;
        std::uint32_t schema_version{1U};
        std::uint64_t schema_hash{};
        EScriptAbilityReceiverKind receiver{EScriptAbilityReceiverKind::NONE};
        std::span<const ScriptAbilityMethodDescription> methods;
    };

    struct ScriptAbilityBinding final
    {
        const ScriptAbilityDescription* description{};
        void* context{};
        const void* dispatch{};
        std::span<const ScriptAbilityErasedMethodBinding> erased_methods;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            if (description == nullptr || !description->id.isValid() ||
                !scriptAbilityCodeNameValid(description->name) || description->schema_hash == 0U || dispatch == nullptr)
                return false;
            return description->receiver == EScriptAbilityReceiverKind::NONE ? context == nullptr : context != nullptr;
        }
    };

    enum class EScriptAbilityBindingError : std::uint8_t
    {
        INVALID_BINDING,
        CONTRACT_MISMATCH,
    };

    template <class Value>
    inline constexpr bool ScriptAbilityValueShapeSupported =
        !std::is_rvalue_reference_v<Value> &&
        !std::is_volatile_v<std::remove_reference_t<Value>> &&
        (!std::is_lvalue_reference_v<Value> || std::is_const_v<std::remove_reference_t<Value>>);

    template <class Value>
        requires lux::semantic::TypeDeclared<std::remove_cvref_t<Value>>
    [[nodiscard]] consteval ScriptAbilityValueDescription makeScriptAbilityValue(
        EScriptAbilityValueLifetime lifetime
    ) noexcept
    {
        static_assert(
            ScriptAbilityValueShapeSupported<Value>,
            "Script Ability values support only value or const lvalue-reference shapes"
        );
        using Type = std::remove_cvref_t<Value>;
        using Traits = lux::semantic::TypeTraits<Type>;
        constexpr auto pass = std::is_lvalue_reference_v<Value>
            ? lux::semantic::EValuePass::CONST_REF
            : lux::semantic::EValuePass::VALUE;
        return {
            lux::semantic::typeId(Traits::CanonicalName),
            Traits::CanonicalName,
            pass,
            Traits::AbiKind,
            Traits::Size,
            Traits::Alignment,
            lifetime
        };
    }

    namespace detail
    {
        struct AbilitySchemaHasher final
        {
            std::uint64_t value{14695981039346656037ULL};

            constexpr void byte(std::uint8_t item) noexcept
            {
                value ^= item;
                value *= 1099511628211ULL;
            }

            constexpr void u32(std::uint32_t item) noexcept
            {
                for (std::uint32_t shift{}; shift < 32U; shift += 8U)
                    byte(static_cast<std::uint8_t>(item >> shift));
            }

            constexpr void u64(std::uint64_t item) noexcept
            {
                for (std::uint32_t shift{}; shift < 64U; shift += 8U)
                    byte(static_cast<std::uint8_t>(item >> shift));
            }

            constexpr void text(std::string_view item) noexcept
            {
                u32(static_cast<std::uint32_t>(item.size()));
                for (const unsigned char character : item)
                    byte(character);
            }
        };

        constexpr void hashValue(AbilitySchemaHasher& hash, const ScriptAbilityValueDescription& value) noexcept
        {
            hash.text(value.canonical_name);
            hash.u64(value.type_id);
            hash.byte(static_cast<std::uint8_t>(value.pass));
            hash.byte(value.abi_kind);
            hash.u32(value.size);
            hash.u32(value.alignment);
            hash.byte(static_cast<std::uint8_t>(value.lifetime));
        }

        [[nodiscard]] constexpr std::size_t canonicalMethodRank(
            std::span<const ScriptAbilityMethodDescription> methods,
            std::size_t candidate
        ) noexcept
        {
            std::size_t rank{};
            for (std::size_t index{}; index < methods.size(); ++index)
            {
                const auto left = methods[index].id.name();
                const auto right = methods[candidate].id.name();
                if (left < right || (left == right && index < candidate))
                    ++rank;
            }
            return rank;
        }
    } // namespace detail

    [[nodiscard]] constexpr bool scriptAbilityMethodIdsUnique(
        std::span<const ScriptAbilityMethodDescription> methods
    ) noexcept
    {
        for (std::size_t left{}; left < methods.size(); ++left)
        {
            if (!methods[left].id.isValid())
                return false;
            for (std::size_t right{left + 1U}; right < methods.size(); ++right)
            {
                if (methods[left].id == methods[right].id)
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr std::uint64_t scriptAbilitySchemaHash(
        ScriptApiContractIdView id,
        EScriptAbilityReceiverKind receiver,
        std::span<const ScriptAbilityMethodDescription> methods,
        std::uint32_t schema_version = 1U
    ) noexcept
    {
        if (!id.isValid() || !scriptAbilityMethodIdsUnique(methods))
            return 0U;

        detail::AbilitySchemaHasher hash;
        hash.u32(schema_version);
        hash.text(id.name());
        hash.byte(static_cast<std::uint8_t>(receiver));
        hash.u32(static_cast<std::uint32_t>(methods.size()));
        for (std::size_t ordinal{}; ordinal < methods.size(); ++ordinal)
        {
            const ScriptAbilityMethodDescription* canonical{};
            for (std::size_t candidate{}; candidate < methods.size(); ++candidate)
            {
                if (detail::canonicalMethodRank(methods, candidate) == ordinal)
                {
                    canonical = std::addressof(methods[candidate]);
                    break;
                }
            }
            if (canonical == nullptr)
                return 0U;
            const auto& method = *canonical;
            hash.text(method.id.name());
            hash.byte(static_cast<std::uint8_t>(method.kind));
            hash.u32(static_cast<std::uint32_t>(method.parameters.size()));
            for (const auto& parameter : method.parameters)
                detail::hashValue(hash, parameter.value);
            hash.u32(static_cast<std::uint32_t>(method.results.size()));
            for (const auto& result : method.results)
                detail::hashValue(hash, result);
        }
        return hash.value == 0U ? 1U : hash.value;
    }

    template <class Ability>
    struct ScriptAbilityTraits;

    template <class Ability>
    class ScriptAbilityCpp;

    template <class Ability, class Context>
    class ScriptAbilityCoroutine;

    template <class Ability, class Provider>
    class ScriptAbilityStatic;

    template <class Ability, class Provider>
    [[nodiscard]] auto bindScriptAbility(Provider& provider) noexcept
        -> decltype(ScriptAbilityTraits<Ability>::bind(provider))
    {
        return ScriptAbilityTraits<Ability>::bind(provider);
    }

    template <class Ability>
    [[nodiscard]] auto bindScriptAbility() noexcept -> decltype(ScriptAbilityTraits<Ability>::bind())
    {
        return ScriptAbilityTraits<Ability>::bind();
    }
} // namespace lux::script
