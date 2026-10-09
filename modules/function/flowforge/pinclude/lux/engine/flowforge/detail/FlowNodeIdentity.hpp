#pragma once

#include <array>
#include <cstdint>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <string_view>

namespace lux::flowforge::detail
{
    // Private compatibility tags for the frozen builtin source encoding. Runtime node definitions
    // and compiler dispatch use registrations; these tags must not become a graph kind API.
    enum class EBuiltinSourceKind : std::uint8_t
    {
        INVALID = 0,
        START = 1,
        BRANCH = 2,
        SEQUENCE = 3,
        FOR_LOOP = 4,
        WHILE_LOOP = 5,
        RETURN = 6,
        BREAK = 7,
        FUNC_DEF_START = 8,
        FUNC_RETURN = 9,
        NATIVE_FUNC_CALL = 10,
        GRAPH_FUNC_CALL = 11,
        SCRIPT_ABILITY_CALL = 12,
        SCRIPT_EVENT_WAIT = 13,
        CREATE_OBJECT = 14,
        GET_OBJECT = 15,
        SET_OBJECT = 16,
        GET_FIELD = 17,
        SET_FIELD = 18,
        ADD = 19,
        SUBTRACT = 20,
        MULTIPLY = 21,
        DIVIDE = 22,
        MODULO = 23,
        LOGICAL_AND = 24,
        LOGICAL_OR = 25,
        LOGICAL_NOT = 26,
        NEGATE = 27,
        CMP_EQ = 28,
        CMP_NE = 29,
        CMP_LT = 30,
        CMP_LE = 31,
        CMP_GT = 32,
        CMP_GE = 33,
        GET_VARIABLE = 34,
        SET_VARIABLE = 35,
        ON_EVENT = 36,
        SEND_EVENT = 37,
        EXTENSION = 255
    };

    struct BuiltinNodeIdentity final
    {
        EBuiltinSourceKind kind;
        std::string_view name;
    };

    inline constexpr std::array builtin_node_identities{
        BuiltinNodeIdentity{EBuiltinSourceKind::INVALID, "lux.flow.invalid"},
        BuiltinNodeIdentity{EBuiltinSourceKind::START, "lux.flow.start"},
        BuiltinNodeIdentity{EBuiltinSourceKind::BRANCH, "lux.flow.branch"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SEQUENCE, "lux.flow.sequence"},
        BuiltinNodeIdentity{EBuiltinSourceKind::FOR_LOOP, "lux.flow.for_loop"},
        BuiltinNodeIdentity{EBuiltinSourceKind::WHILE_LOOP, "lux.flow.while_loop"},
        BuiltinNodeIdentity{EBuiltinSourceKind::RETURN, "lux.flow.return"},
        BuiltinNodeIdentity{EBuiltinSourceKind::BREAK, "lux.flow.break"},
        BuiltinNodeIdentity{EBuiltinSourceKind::FUNC_DEF_START, "lux.flow.function"},
        BuiltinNodeIdentity{EBuiltinSourceKind::FUNC_RETURN, "lux.flow.function_return"},
        BuiltinNodeIdentity{EBuiltinSourceKind::NATIVE_FUNC_CALL, "lux.flow.native_call"},
        BuiltinNodeIdentity{EBuiltinSourceKind::GRAPH_FUNC_CALL, "lux.flow.function_call"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SCRIPT_ABILITY_CALL, "lux.flow.ability_call"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SCRIPT_EVENT_WAIT, "lux.flow.event_wait"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CREATE_OBJECT, "lux.flow.create_object"},
        BuiltinNodeIdentity{EBuiltinSourceKind::GET_OBJECT, "lux.flow.get_object"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SET_OBJECT, "lux.flow.set_object"},
        BuiltinNodeIdentity{EBuiltinSourceKind::GET_FIELD, "lux.flow.get_field"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SET_FIELD, "lux.flow.set_field"},
        BuiltinNodeIdentity{EBuiltinSourceKind::ADD, "lux.flow.add"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SUBTRACT, "lux.flow.subtract"},
        BuiltinNodeIdentity{EBuiltinSourceKind::MULTIPLY, "lux.flow.multiply"},
        BuiltinNodeIdentity{EBuiltinSourceKind::DIVIDE, "lux.flow.divide"},
        BuiltinNodeIdentity{EBuiltinSourceKind::MODULO, "lux.flow.modulo"},
        BuiltinNodeIdentity{EBuiltinSourceKind::LOGICAL_AND, "lux.flow.and"},
        BuiltinNodeIdentity{EBuiltinSourceKind::LOGICAL_OR, "lux.flow.or"},
        BuiltinNodeIdentity{EBuiltinSourceKind::LOGICAL_NOT, "lux.flow.not"},
        BuiltinNodeIdentity{EBuiltinSourceKind::NEGATE, "lux.flow.negate"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CMP_EQ, "lux.flow.equal"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CMP_NE, "lux.flow.not_equal"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CMP_LT, "lux.flow.less"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CMP_LE, "lux.flow.less_equal"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CMP_GT, "lux.flow.greater"},
        BuiltinNodeIdentity{EBuiltinSourceKind::CMP_GE, "lux.flow.greater_equal"},
        BuiltinNodeIdentity{EBuiltinSourceKind::GET_VARIABLE, "lux.flow.get_variable"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SET_VARIABLE, "lux.flow.set_variable"},
        BuiltinNodeIdentity{EBuiltinSourceKind::ON_EVENT, "lux.flow.event"},
        BuiltinNodeIdentity{EBuiltinSourceKind::SEND_EVENT, "lux.flow.send_event"}
    };

    [[nodiscard]] constexpr EBuiltinSourceKind builtinSourceKind(std::string_view name) noexcept
    {
        for (const auto& entry : builtin_node_identities)
        {
            if (entry.name == name)
            {
                return entry.kind;
            }
        }
        return EBuiltinSourceKind::EXTENSION;
    }

    [[nodiscard]] constexpr graph::PinSemanticId builtinPinSemantic(
        EFlowSourcePinKind kind,
        std::size_t ordinal
    ) noexcept
    {
        switch (kind)
        {
        case EFlowSourcePinKind::EXEC_IN:
            return pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::INPUT, ordinal);
        case EFlowSourcePinKind::EXEC_OUT:
            return pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, ordinal);
        case EFlowSourcePinKind::DATA_IN:
            return pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::INPUT, ordinal);
        case EFlowSourcePinKind::DATA_OUT:
            return pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::OUTPUT, ordinal);
        default:
            return {};
        }
    }

    [[nodiscard]] constexpr bool isScalarSourceKind(EBuiltinSourceKind kind) noexcept
    {
        const bool is_arithmetic = kind >= EBuiltinSourceKind::ADD && kind <= EBuiltinSourceKind::NEGATE;
        const bool is_comparison = kind >= EBuiltinSourceKind::CMP_EQ && kind <= EBuiltinSourceKind::CMP_GE;
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
