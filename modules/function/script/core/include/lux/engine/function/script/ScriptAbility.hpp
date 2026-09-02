#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/function/script/ScriptApi.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace lux::script
{
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

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            if (description == nullptr || !description->id.isValid() || description->schema_hash == 0U ||
                dispatch == nullptr)
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
        requires lux::semantic::TypeDeclared<std::remove_cvref_t<Value>>
    [[nodiscard]] consteval ScriptAbilityValueDescription makeScriptAbilityValue(
        EScriptAbilityValueLifetime lifetime
    ) noexcept
    {
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
    } // namespace detail

    [[nodiscard]] constexpr std::uint64_t scriptAbilitySchemaHash(
        ScriptApiContractIdView id,
        EScriptAbilityReceiverKind receiver,
        std::span<const ScriptAbilityMethodDescription> methods,
        std::uint32_t schema_version = 1U
    ) noexcept
    {
        detail::AbilitySchemaHasher hash;
        hash.u32(schema_version);
        hash.text(id.name());
        hash.byte(static_cast<std::uint8_t>(receiver));
        hash.u32(static_cast<std::uint32_t>(methods.size()));
        for (const auto& method : methods)
        {
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
