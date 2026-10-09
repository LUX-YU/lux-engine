#pragma once

#include <array>
#include <lux/engine/flowforge/graph/NodeBase.hpp>
#include <string_view>

namespace lux::flowforge::detail
{
    // Array order is the frozen v1 file ordinal, independent of future ENodeOperation numbering.
    // New source and topology use stable names. These builtin adapters will move to their real
    // registrations with the remaining payload migration; they are not an extensible compiler.
    struct BuiltinNodeIdentity final
    {
        ENodeOperation operation;
        std::string_view name;
    };

    inline constexpr std::array builtin_node_identities{
        BuiltinNodeIdentity{ENodeOperation::INVALID, "lux.flow.invalid"},
        BuiltinNodeIdentity{ENodeOperation::START, "lux.flow.start"},
        BuiltinNodeIdentity{ENodeOperation::BRANCH, "lux.flow.branch"},
        BuiltinNodeIdentity{ENodeOperation::SEQUENCE, "lux.flow.sequence"},
        BuiltinNodeIdentity{ENodeOperation::FOR_LOOP, "lux.flow.for_loop"},
        BuiltinNodeIdentity{ENodeOperation::WHILE_LOOP, "lux.flow.while_loop"},
        BuiltinNodeIdentity{ENodeOperation::RETURN, "lux.flow.return"},
        BuiltinNodeIdentity{ENodeOperation::BREAK, "lux.flow.break"},
        BuiltinNodeIdentity{ENodeOperation::FUNC_DEF_START, "lux.flow.function"},
        BuiltinNodeIdentity{ENodeOperation::FUNC_RETURN, "lux.flow.function_return"},
        BuiltinNodeIdentity{ENodeOperation::NATIVE_FUNC_CALL, "lux.flow.native_call"},
        BuiltinNodeIdentity{ENodeOperation::GRAPH_FUNC_CALL, "lux.flow.function_call"},
        BuiltinNodeIdentity{ENodeOperation::SCRIPT_ABILITY_CALL, "lux.flow.ability_call"},
        BuiltinNodeIdentity{ENodeOperation::SCRIPT_EVENT_WAIT, "lux.flow.event_wait"},
        BuiltinNodeIdentity{ENodeOperation::CREATE_OBJECT, "lux.flow.create_object"},
        BuiltinNodeIdentity{ENodeOperation::GET_OBJECT, "lux.flow.get_object"},
        BuiltinNodeIdentity{ENodeOperation::SET_OBJECT, "lux.flow.set_object"},
        BuiltinNodeIdentity{ENodeOperation::GET_FIELD, "lux.flow.get_field"},
        BuiltinNodeIdentity{ENodeOperation::SET_FIELD, "lux.flow.set_field"},
        BuiltinNodeIdentity{ENodeOperation::ADD, "lux.flow.add"},
        BuiltinNodeIdentity{ENodeOperation::SUBTRACT, "lux.flow.subtract"},
        BuiltinNodeIdentity{ENodeOperation::MULTIPLY, "lux.flow.multiply"},
        BuiltinNodeIdentity{ENodeOperation::DIVIDE, "lux.flow.divide"},
        BuiltinNodeIdentity{ENodeOperation::MODULO, "lux.flow.modulo"},
        BuiltinNodeIdentity{ENodeOperation::LOGICAL_AND, "lux.flow.and"},
        BuiltinNodeIdentity{ENodeOperation::LOGICAL_OR, "lux.flow.or"},
        BuiltinNodeIdentity{ENodeOperation::LOGICAL_NOT, "lux.flow.not"},
        BuiltinNodeIdentity{ENodeOperation::NEGATE, "lux.flow.negate"},
        BuiltinNodeIdentity{ENodeOperation::CMP_EQ, "lux.flow.equal"},
        BuiltinNodeIdentity{ENodeOperation::CMP_NE, "lux.flow.not_equal"},
        BuiltinNodeIdentity{ENodeOperation::CMP_LT, "lux.flow.less"},
        BuiltinNodeIdentity{ENodeOperation::CMP_LE, "lux.flow.less_equal"},
        BuiltinNodeIdentity{ENodeOperation::CMP_GT, "lux.flow.greater"},
        BuiltinNodeIdentity{ENodeOperation::CMP_GE, "lux.flow.greater_equal"},
        BuiltinNodeIdentity{ENodeOperation::GET_VARIABLE, "lux.flow.get_variable"},
        BuiltinNodeIdentity{ENodeOperation::SET_VARIABLE, "lux.flow.set_variable"},
        BuiltinNodeIdentity{ENodeOperation::ON_EVENT, "lux.flow.event"},
        BuiltinNodeIdentity{ENodeOperation::SEND_EVENT, "lux.flow.send_event"}
    };

    [[nodiscard]] constexpr std::string_view builtinNodeName(ENodeOperation operation) noexcept
    {
        for (const auto& entry : builtin_node_identities)
        {
            if (entry.operation == operation)
            {
                return entry.name;
            }
        }
        return {};
    }

    [[nodiscard]] constexpr ENodeOperation builtinNodeOperation(std::string_view name) noexcept
    {
        for (const auto& entry : builtin_node_identities)
        {
            if (entry.name == name)
            {
                return entry.operation;
            }
        }
        return ENodeOperation::REGISTERED_VALUE;
    }

    [[nodiscard]] constexpr graph::PinSemanticId builtinPinSemantic(EPinKind kind, std::size_t ordinal) noexcept
    {
        return graph::PinSemanticId{((static_cast<std::uint64_t>(kind) + 1U) << 56U) | (ordinal + 1U)};
    }

    [[nodiscard]] constexpr bool registeredScalarOperation(ENodeOperation operation) noexcept
    {
        const bool is_arithmetic = operation >= ENodeOperation::ADD && operation <= ENodeOperation::NEGATE;
        const bool is_comparison = operation >= ENodeOperation::CMP_EQ && operation <= ENodeOperation::CMP_GE;
        return is_arithmetic || is_comparison;
    }

    [[nodiscard]] constexpr bool canonicalNodeName(std::string_view name) noexcept
    {
        const auto initial = [](char c) noexcept
        { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        if (name.empty() || !initial(name.front()))
        {
            return false;
        }
        for (const char c : name)
        {
            const bool is_valid = initial(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!is_valid)
            {
                return false;
            }
        }
        return true;
    }
} // namespace lux::flowforge::detail
