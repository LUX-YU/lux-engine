#pragma once

#include <lux/engine/function/script/ScriptSemantic.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::simulation
{
    enum class ESystemHookCardinality : std::uint8_t
    {
        SINGLE,
        MULTI,
    };

    struct SystemHookPoint final
    {
        std::string_view name;
        ESystemHookCardinality cardinality{ESystemHookCardinality::MULTI};
        lux::script::ScriptFunctionSignatureView signature;
    };

    namespace detail
    {
        template <class Type>
        inline constexpr bool kValidHookParameter = [] {
            using Value = std::remove_reference_t<Type>;
            using Base = std::remove_cv_t<Value>;
            if constexpr (
                !lux::script::ScriptSemanticTypeDeclared<Base> || std::is_pointer_v<Base> ||
                std::is_rvalue_reference_v<Type>)
            {
                return false;
            }
            else if constexpr (std::is_lvalue_reference_v<Type>)
            {
                return std::is_const_v<Value>;
            }
            else
            {
                return true;
            }
        }();

        template <class Type> [[nodiscard]] consteval lux::script::ScriptSemanticType hookParameterType() noexcept
        {
            static_assert(kValidHookParameter<Type>);
            using Value = std::remove_cv_t<std::remove_reference_t<Type>>;
            constexpr auto pass = std::is_lvalue_reference_v<Type> ? lux::script::EScriptPassMode::CONST_REF
                                                                   : lux::script::EScriptPassMode::VALUE;
            return lux::script::makeScriptSemanticType<Value>(pass);
        }

        template <class Type>
        inline constexpr bool kValidHookReturn =
            std::is_void_v<Type> || (lux::script::ScriptSemanticTypeDeclared<std::remove_cv_t<Type>> &&
                                     !std::is_reference_v<Type> && !std::is_pointer_v<Type>);

        template <class Signature> struct SystemHookSignatureStorage;

        template <class Return, class... Parameters> struct SystemHookSignatureStorage<Return(Parameters...)> final
        {
            static_assert((kValidHookParameter<Parameters> && ...));
            static_assert(kValidHookReturn<Return>);

            inline static constexpr std::array<lux::script::ScriptSemanticType, sizeof...(Parameters)> parameter_types{
                hookParameterType<Parameters>()...};
            inline static constexpr auto return_types = [] {
                if constexpr (std::is_void_v<Return>)
                    return std::array<lux::script::ScriptSemanticType, 0U>{};
                else
                    return std::array{lux::script::makeScriptSemanticType<std::remove_cv_t<Return>>()};
            }();

            [[nodiscard]] static constexpr lux::script::ScriptFunctionSignatureView view() noexcept
            {
                return {
                    std::span<const lux::script::ScriptSemanticType>{parameter_types},
                    std::span<const lux::script::ScriptSemanticType>{return_types}};
            }
        };

        template <class Return, class... Parameters>
        struct SystemHookSignatureStorage<Return(Parameters...) noexcept> final
            : SystemHookSignatureStorage<Return(Parameters...)>
        {
        };
    }

    template <class Signature>
    [[nodiscard]] consteval SystemHookPoint makeSystemHookPoint(
        std::string_view name,
        ESystemHookCardinality cardinality = ESystemHookCardinality::MULTI
    ) noexcept
    {
        return SystemHookPoint{name, cardinality, detail::SystemHookSignatureStorage<Signature>::view()};
    }
}
