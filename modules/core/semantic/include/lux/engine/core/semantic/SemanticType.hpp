#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace lux::semantic
{
    using TypeId = std::uint64_t;
    inline constexpr TypeId InvalidTypeId{};

    enum class EValuePass : std::uint8_t
    {
        VALUE,
        CONST_REF,
    };

    enum class EAbiKind : std::uint8_t
    {
        VOID = 0U,
        BOOL = 1U,
        I32 = 2U,
        U32 = 3U,
        I64 = 4U,
        U64 = 5U,
        F32 = 6U,
        F64 = 7U,
        STRUCT_REF = 8U,
    };

    [[nodiscard]] constexpr TypeId typeId(
        std::string_view canonical_name
    ) noexcept
    {
        std::uint64_t result = 14695981039346656037ULL;
        for (const auto value : canonical_name)
        {
            result ^= static_cast<std::uint8_t>(value);
            result *= 1099511628211ULL;
        }
        return result == InvalidTypeId ? 1U : result;
    }

    struct Type final
    {
        TypeId type_id{};
        std::string_view canonical_name;
        EValuePass pass{EValuePass::VALUE};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return type_id != InvalidTypeId && !canonical_name.empty() &&
                type_id == typeId(canonical_name);
        }

        friend constexpr bool operator==(const Type&, const Type&) noexcept =
            default;
    };

    struct SignatureView final
    {
        std::span<const Type> parameters;
        std::span<const Type> returns;
    };

    struct Layout final
    {
        TypeId type_id{};
        std::string_view canonical_name;
        std::uint8_t abi_kind{static_cast<std::uint8_t>(EAbiKind::VOID)};
        std::uint32_t size{};
        std::uint32_t alignment{};
    };

    template <class Value>
    struct TypeTraits;

#define LUX_SEMANTIC_BUILTIN(tag, cpp_type, canonical, abi_kind_value)         \
    template <>                                                                \
    struct TypeTraits<cpp_type> final                                           \
    {                                                                          \
        inline static constexpr std::string_view CanonicalName = canonical;    \
        inline static constexpr std::uint8_t AbiKind = abi_kind_value;         \
        inline static constexpr std::uint32_t Size = sizeof(cpp_type);         \
        inline static constexpr std::uint32_t Alignment = alignof(cpp_type);   \
    };

#include <lux/engine/core/semantic/SemanticBuiltin.def>

#undef LUX_SEMANTIC_BUILTIN

    template <class Value>
    concept TypeDeclared = requires
    {
        { TypeTraits<std::remove_cv_t<Value>>::CanonicalName }
            -> std::convertible_to<std::string_view>;
        { TypeTraits<std::remove_cv_t<Value>>::AbiKind }
            -> std::convertible_to<std::uint8_t>;
    };

    template <class Value>
        requires TypeDeclared<Value>
    [[nodiscard]] consteval Type makeType(
        EValuePass pass = EValuePass::VALUE
    ) noexcept
    {
        constexpr auto name =
            TypeTraits<std::remove_cv_t<Value>>::CanonicalName;
        return Type{typeId(name), name, pass};
    }

    inline constexpr auto BuiltinLayouts = std::array{
#define LUX_SEMANTIC_BUILTIN(tag, cpp_type, canonical, abi_kind_value)         \
        Layout{                                                                \
            typeId(canonical),                                                 \
            canonical,                                                         \
            abi_kind_value,                                                    \
            sizeof(cpp_type),                                                  \
            alignof(cpp_type)},
#include <lux/engine/core/semantic/SemanticBuiltin.def>
#undef LUX_SEMANTIC_BUILTIN
    };

    [[nodiscard]] constexpr const Layout* builtinLayout(TypeId id) noexcept
    {
        for (const auto& layout : BuiltinLayouts)
        {
            if (layout.type_id == id)
                return &layout;
        }
        return nullptr;
    }

    [[nodiscard]] constexpr bool sameSignature(
        SignatureView left,
        SignatureView right
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
}
