#pragma once
#include <lux/engine/description/Script.hpp>

namespace lux::simulation::script
{
    [[nodiscard]] inline bool sameScriptType(const lux::rdesc::ScriptValueType& value,
                                             const lux::semantic::Type& expected) noexcept
    {
        return value.type_id == expected.type_id && value.canonical_name == expected.canonical_name &&
               value.pass == expected.pass;
    }

    template <class ParameterAt>
    [[nodiscard]] bool sameScriptHookSignature(const lux::rdesc::ScriptFunction& function, std::size_t count,
                                               ParameterAt parameter) noexcept
    {
        if (!function.returns.empty() || function.args.size() != count)
            return false;
        for (std::size_t index{}; index < count; ++index)
            if (!sameScriptType(function.args[index], parameter(index)))
                return false;
        return true;
    }

    [[nodiscard]] inline bool sameScriptHookSignature(const lux::rdesc::ScriptFunction& function,
                                                      lux::semantic::SignatureView signature) noexcept
    {
        return sameScriptHookSignature(function, signature.parameters.size(),
                                       [signature](std::size_t index) noexcept { return signature.parameters[index]; });
    }

    [[nodiscard]] inline bool sameScriptEventSignature(const lux::rdesc::ScriptFunction& function,
                                                       const lux::semantic::Type& payload) noexcept
    {
        return function.returns.empty() && function.args.size() == 1U && sameScriptType(function.args.front(), payload);
    }
} // namespace lux::simulation::script
