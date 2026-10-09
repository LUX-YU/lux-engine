#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <string>

namespace lux::flowforge::detail
{
    // Stable, owned reflection metadata for one ABI value. Never carries graph or pin identity.
    struct ScriptValueType final
    {
        std::string name;
        meta::RefType type;

        explicit ScriptValueType(const script::ScriptAbilityValueDescription& description) noexcept;
        ScriptValueType(const ScriptValueType&) = delete;
        ScriptValueType& operator=(const ScriptValueType&) = delete;
        ScriptValueType(ScriptValueType&&) = delete;
        ScriptValueType& operator=(ScriptValueType&&) = delete;
    };
} // namespace lux::flowforge::detail
