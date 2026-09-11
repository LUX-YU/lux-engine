#pragma once

#include <lux/engine/simulation/scripting/ScriptSyncStep.hpp>

#include <array>
#include <memory>

#include <type_traits>

namespace lux::simulation::script::detail
{
    template<class Signature>
    struct ScriptSyncStepCall;

    template <class T> [[nodiscard]] consteval ScriptSyncStepType syncStepType() noexcept
    {
        using Value = std::remove_cvref_t<T>;
        using Traits = lux::semantic::TypeTraits<Value>;
        static_assert(lux::semantic::TypeDeclared<Value>);
        static_assert(Traits::Size == sizeof(Value) && Traits::Alignment == alignof(Value));
        return {{lux::semantic::typeId(Traits::CanonicalName), Traits::CanonicalName, Traits::AbiKind, Traits::Size,
                 Traits::Alignment},
                std::is_reference_v<T> ? lux::semantic::EValuePass::CONST_REF : lux::semantic::EValuePass::VALUE};
    }

    template<class R, class... Args>
    struct ScriptSyncStepCall<R(Args...)>
    {
        using Result = lux::cxx::expected<R, ScriptSyncStepError>;
        static_assert(sizeof...(Args) <= 64U);
        static_assert(((!std::is_pointer_v<std::remove_cvref_t<Args>> &&
            !std::is_volatile_v<std::remove_reference_t<Args>> &&
            (!std::is_reference_v<Args> || (std::is_lvalue_reference_v<Args> &&
                std::is_const_v<std::remove_reference_t<Args>>))) && ...));
        inline static constexpr std::array<ScriptSyncStepType, sizeof...(Args)> Arguments{syncStepType<Args>()...};
        inline static constexpr auto Results = []() consteval
        {
            if constexpr (std::is_void_v<R>)
                return std::array<ScriptSyncStepType, 0>{};
            else
            {
                static_assert(!std::is_reference_v<R> && std::is_trivially_copyable_v<R>);
                constexpr auto kind = lux::semantic::TypeTraits<R>::AbiKind;
                static_assert(kind == LUX_SCRIPT_VK_BOOL || kind == LUX_SCRIPT_VK_INT32 ||
                    kind == LUX_SCRIPT_VK_UINT32 || kind == LUX_SCRIPT_VK_FLOAT || kind == LUX_SCRIPT_VK_DOUBLE,
                    "Synchronous script steps currently return scalar or void only");
                return std::array{syncStepType<R>()};
            }
        }();
        inline static constexpr ScriptSyncStepShape Shape{Arguments, Results};

        template <class Context>
        [[nodiscard]] static Result invoke(Context& context, std::uint32_t ordinal,
                                           const std::remove_cvref_t<Args>&... values) noexcept
        {
            const std::array<const void*, sizeof...(Args)> arguments{std::addressof(values)...};
            if constexpr (std::is_void_v<R>)
            {
                return context.invokeSyncStep(ordinal, Shape, arguments.data(), nullptr);
            }
            else
            {
                R value;
                const auto invoked = context.invokeSyncStep(ordinal, Shape, arguments.data(), &value);
                if (!invoked) return lux::cxx::unexpected<ScriptSyncStepError>(invoked.error());
                return value;
            }
        }
    };

    template <class Owner>
    [[nodiscard]] consteval std::span<const ScriptSyncStepShape* const> cppStaticSyncStepShapes() noexcept
    {
        if constexpr (requires { Owner::SyncStepShapes; })
            return Owner::SyncStepShapes;
        else
            return {};
    }
} // namespace lux::simulation::script::detail

namespace lux::simulation::script
{
    template <class Signature> [[nodiscard]] consteval const ScriptSyncStepShape* scriptSyncStepShape() noexcept
    {
        return &detail::ScriptSyncStepCall<Signature>::Shape;
    }
} // namespace lux::simulation::script
