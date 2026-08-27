#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdint>
#include <string_view>

namespace lux::script
{
    using ScriptSymbolId = std::uint64_t;
    inline constexpr ScriptSymbolId InvalidScriptSymbolId = 0U;

    using EScriptPassMode = lux::semantic::EValuePass;

    [[nodiscard]] constexpr std::uint64_t scriptSemanticTypeId(
        std::string_view canonical_name
    ) noexcept
    {
        return lux::semantic::typeId(canonical_name);
    }

    [[nodiscard]] constexpr ScriptSymbolId scriptSymbolId(
        std::string_view canonical_symbol_identity
    ) noexcept
    {
        const auto value = scriptSemanticTypeId(canonical_symbol_identity);
        return value == InvalidScriptSymbolId ? 1U : value;
    }

    using ScriptSemanticType = lux::semantic::Type;
    using ScriptFunctionSignatureView = lux::semantic::SignatureView;

    [[nodiscard]] constexpr ScriptSymbolId scriptSymbolId(
        std::string_view declaring_scope,
        std::string_view function_name,
        ScriptFunctionSignatureView signature
    ) noexcept
    {
        std::uint64_t result = 14695981039346656037ULL;
        const auto append = [&result](std::string_view value) noexcept
        {
            for (const auto character : value)
            {
                result ^= static_cast<std::uint8_t>(character);
                result *= 1099511628211ULL;
            }
            result ^= 0xFFU;
            result *= 1099511628211ULL;
        };
        append(declaring_scope);
        append(function_name);
        for (const auto& parameter : signature.parameters)
        {
            append(parameter.canonical_name);
            result ^= static_cast<std::uint8_t>(parameter.pass);
            result *= 1099511628211ULL;
        }
        result ^= 0xFEU;
        result *= 1099511628211ULL;
        for (const auto& return_type : signature.returns)
        {
            append(return_type.canonical_name);
            result ^= static_cast<std::uint8_t>(return_type.pass);
            result *= 1099511628211ULL;
        }
        return result == InvalidScriptSymbolId ? 1U : result;
    }

    [[nodiscard]] constexpr bool sameScriptSignature(
        ScriptFunctionSignatureView left,
        ScriptFunctionSignatureView right
    ) noexcept
    {
        return lux::semantic::sameSignature(left, right);
    }

    template <class Type>
    struct ScriptSemanticTypeTraits
    {
    };

#define LUX_SEMANTIC_BUILTIN(tag, cpp_type, canonical, abi_kind_value)         \
    template <>                                                                \
    struct ScriptSemanticTypeTraits<cpp_type> final                            \
    {                                                                          \
        inline static constexpr std::string_view CanonicalName = canonical;    \
        inline static constexpr std::uint8_t AbiKind = abi_kind_value;         \
        inline static constexpr std::uint32_t Size = sizeof(cpp_type);         \
        inline static constexpr std::uint32_t Alignment = alignof(cpp_type);   \
    };

#include <lux/engine/core/semantic/SemanticBuiltin.def>

#undef LUX_SEMANTIC_BUILTIN

    template <class Type>
    concept ScriptSemanticTypeDeclared = requires
    {
        { ScriptSemanticTypeTraits<std::remove_cv_t<Type>>::CanonicalName }
            -> std::convertible_to<std::string_view>;
    };

    template <class Type>
        requires ScriptSemanticTypeDeclared<Type>
    [[nodiscard]] consteval ScriptSemanticType makeScriptSemanticType(
        EScriptPassMode pass = EScriptPassMode::VALUE
    ) noexcept
    {
        constexpr auto name =
            ScriptSemanticTypeTraits<std::remove_cv_t<Type>>::CanonicalName;
        return ScriptSemanticType{scriptSemanticTypeId(name), name, pass};
    }

    using ScriptSemanticLayout = lux::semantic::Layout;
    inline constexpr auto ScriptBuiltinSemanticLayouts =
        lux::semantic::BuiltinLayouts;

    [[nodiscard]] constexpr const ScriptSemanticLayout* scriptBuiltinLayout(
        std::uint64_t type_id
    ) noexcept
    {
        return lux::semantic::builtinLayout(type_id);
    }
}
