#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

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

#define LUX_SCRIPT_SEMANTIC_TYPE(cpp_type, canonical)                          \
    template <>                                                                \
    struct ScriptSemanticTypeTraits<cpp_type> final                            \
    {                                                                          \
        inline static constexpr std::string_view CanonicalName = canonical;    \
    }

    LUX_SCRIPT_SEMANTIC_TYPE(bool, "lux.bool");
    LUX_SCRIPT_SEMANTIC_TYPE(std::int32_t, "lux.i32");
    LUX_SCRIPT_SEMANTIC_TYPE(std::uint32_t, "lux.u32");
    LUX_SCRIPT_SEMANTIC_TYPE(std::int64_t, "lux.i64");
    LUX_SCRIPT_SEMANTIC_TYPE(std::uint64_t, "lux.u64");
    LUX_SCRIPT_SEMANTIC_TYPE(float, "lux.f32");
    LUX_SCRIPT_SEMANTIC_TYPE(double, "lux.f64");

#undef LUX_SCRIPT_SEMANTIC_TYPE

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
}
