#pragma once

#include <lux/engine/simulation/scripting/ScriptSyncStep.hpp>

#include <type_traits>

namespace lux::simulation::script::detail
{
    template<class Signature>
    struct ScriptSyncStepCall;

    template<class T>
    [[nodiscard]] lux_script_value_slot syncStepSlot(const T& value) noexcept
    {
        using Value = std::remove_cvref_t<T>;
        using Traits = lux::semantic::TypeTraits<Value>;
        static_assert(lux::semantic::TypeDeclared<Value>);
        static_assert(Traits::Size == sizeof(Value) && Traits::Alignment == alignof(Value));
        return {Traits::AbiKind, {}, Traits::Size, lux::semantic::typeId(Traits::CanonicalName),
            const_cast<Value*>(std::addressof(value))};
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
        inline static constexpr std::array<lux::semantic::EValuePass, sizeof...(Args)> Passes{
            (std::is_reference_v<Args> ? lux::semantic::EValuePass::CONST_REF : lux::semantic::EValuePass::VALUE)...
        };

        template<class Context>
        [[nodiscard]] static Result invoke(Context& context, std::uint32_t ordinal,
            const std::remove_cvref_t<Args>&... values) noexcept
        {
            const std::array<lux_script_value_slot, sizeof...(Args)> arguments{syncStepSlot(values)...};
            lux_script_call_frame frame{};
            frame.args = arguments.data();
            frame.arg_count = static_cast<std::uint32_t>(arguments.size());
            if constexpr (std::is_void_v<R>)
            {
                return context.invokeSyncStep(ordinal, frame, Passes);
            }
            else
            {
                static_assert(!std::is_reference_v<R> && std::is_trivially_copyable_v<R>);
                constexpr auto kind = lux::semantic::TypeTraits<R>::AbiKind;
                static_assert(kind == LUX_SCRIPT_VK_BOOL || kind == LUX_SCRIPT_VK_INT32 ||
                    kind == LUX_SCRIPT_VK_UINT32 || kind == LUX_SCRIPT_VK_FLOAT || kind == LUX_SCRIPT_VK_DOUBLE,
                    "Synchronous script steps currently return scalar or void only");
                R value;
                auto slot = syncStepSlot(value);
                frame.returns = &slot;
                frame.return_count = 1U;
                const auto invoked = context.invokeSyncStep(ordinal, frame, Passes);
                if (!invoked) return lux::cxx::unexpected<ScriptSyncStepError>(invoked.error());
                return value;
            }
        }
    };
}
