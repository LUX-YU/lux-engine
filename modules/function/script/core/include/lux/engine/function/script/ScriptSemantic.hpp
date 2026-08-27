#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include <lux/engine/function/script/abi/lux_script_abi.h>

namespace lux::script
{
    using ScriptSymbolId = std::uint64_t;
    inline constexpr ScriptSymbolId InvalidScriptSymbolId = 0U;

    enum class EScriptPassMode : std::uint8_t
    {
        VALUE,
        CONST_REF,
    };

    [[nodiscard]] constexpr std::uint64_t scriptSemanticTypeId(
        std::string_view canonical_name
    ) noexcept
    {
        std::uint64_t result = 14695981039346656037ULL;
        for (const auto value : canonical_name)
        {
            result ^= static_cast<std::uint8_t>(value);
            result *= 1099511628211ULL;
        }
        return result;
    }

    [[nodiscard]] constexpr ScriptSymbolId scriptSymbolId(
        std::string_view canonical_symbol_identity
    ) noexcept
    {
        const auto value = scriptSemanticTypeId(canonical_symbol_identity);
        return value == InvalidScriptSymbolId ? 1U : value;
    }

    struct ScriptSemanticType final
    {
        std::uint64_t type_id{};
        std::string_view canonical_name;
        EScriptPassMode pass{EScriptPassMode::VALUE};

        [[nodiscard]] friend constexpr bool operator==(
            const ScriptSemanticType&,
            const ScriptSemanticType&
        ) noexcept = default;
    };

    struct ScriptFunctionSignatureView final
    {
        std::span<const ScriptSemanticType> parameters;
        std::span<const ScriptSemanticType> returns;
    };

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
        if (left.parameters.size() != right.parameters.size() ||
            left.returns.size() != right.returns.size())
        {
            return false;
        }
        for (std::size_t index{}; index < left.parameters.size(); ++index)
        {
            if (left.parameters[index] != right.parameters[index])
                return false;
        }
        for (std::size_t index{}; index < left.returns.size(); ++index)
        {
            if (left.returns[index] != right.returns[index])
                return false;
        }
        return true;
    }

    template <class Type>
    struct ScriptSemanticTypeTraits;

#define LUX_SCRIPT_BUILTIN(tag, cpp_type, canonical, abi_kind_value)           \
    template <>                                                                \
    struct ScriptSemanticTypeTraits<cpp_type> final                            \
    {                                                                          \
        inline static constexpr std::string_view CanonicalName = canonical;    \
        inline static constexpr std::uint8_t AbiKind = abi_kind_value;         \
        inline static constexpr std::uint32_t Size = sizeof(cpp_type);         \
        inline static constexpr std::uint32_t Alignment = alignof(cpp_type);   \
    };

#include <lux/engine/function/script/ScriptSemanticBuiltin.def>

#undef LUX_SCRIPT_BUILTIN

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

    struct ScriptSemanticLayout final
    {
        std::uint64_t type_id{};
        std::string_view canonical_name;
        std::uint8_t abi_kind{LUX_SCRIPT_VK_VOID};
        std::uint32_t size{};
        std::uint32_t alignment{};
    };

    inline constexpr auto ScriptBuiltinSemanticLayouts = std::array{
#define LUX_SCRIPT_BUILTIN(tag, cpp_type, canonical, abi_kind_value)           \
        ScriptSemanticLayout{                                                  \
            scriptSemanticTypeId(canonical), canonical, abi_kind_value,        \
            sizeof(cpp_type), alignof(cpp_type)},
#include <lux/engine/function/script/ScriptSemanticBuiltin.def>
#undef LUX_SCRIPT_BUILTIN
    };

    [[nodiscard]] constexpr const ScriptSemanticLayout* scriptBuiltinLayout(
        std::uint64_t type_id
    ) noexcept
    {
        for (const auto& layout : ScriptBuiltinSemanticLayouts)
        {
            if (layout.type_id == type_id)
                return &layout;
        }
        return nullptr;
    }
}
