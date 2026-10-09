#pragma once

#include <cstdint>
#include <optional>

namespace lux::meta
{
    struct RefType;
}

namespace lux::flowforge
{
    enum class ENodeOperation : std::uint8_t;
}

namespace lux::flowforge::detail
{
    // Compiler-local instructions, never graph type identities or serialized values.
    // The domain selects signedness and ordered floating comparison semantics;
    // the backend only translates the selected instruction to its own IR.
    enum class EScalarInstruction : std::uint8_t
    {
        ADD_INTEGER,
        ADD_FLOAT,
        SUBTRACT_INTEGER,
        SUBTRACT_FLOAT,
        MULTIPLY_INTEGER,
        MULTIPLY_FLOAT,
        DIVIDE_SIGNED,
        DIVIDE_UNSIGNED,
        DIVIDE_FLOAT,
        REMAINDER_SIGNED,
        REMAINDER_UNSIGNED,
        REMAINDER_FLOAT,
        AND,
        OR,
        EQUAL_INTEGER,
        NOT_EQUAL_INTEGER,
        LESS_SIGNED,
        LESS_UNSIGNED,
        LESS_EQUAL_SIGNED,
        LESS_EQUAL_UNSIGNED,
        GREATER_SIGNED,
        GREATER_UNSIGNED,
        GREATER_EQUAL_SIGNED,
        GREATER_EQUAL_UNSIGNED,
        EQUAL_ORDERED_FLOAT,
        NOT_EQUAL_ORDERED_FLOAT,
        LESS_ORDERED_FLOAT,
        LESS_EQUAL_ORDERED_FLOAT,
        GREATER_ORDERED_FLOAT,
        GREATER_EQUAL_ORDERED_FLOAT,
        NEGATE_INTEGER,
        NEGATE_FLOAT,
        NOT_BOOLEAN
    };

    [[nodiscard]] bool isUnsignedScalar(const meta::RefType&) noexcept;
    [[nodiscard]] bool isFloatingScalar(const meta::RefType&) noexcept;

    // Operand type validation/coercion stays with the original compiler boundary.
    // An operation of the wrong arity has no scalar instruction.
    [[nodiscard]] std::optional<EScalarInstruction>
    selectBinaryScalarInstruction(ENodeOperation, const meta::RefType&) noexcept;

    [[nodiscard]] std::optional<EScalarInstruction>
    selectUnaryScalarInstruction(ENodeOperation, const meta::RefType&) noexcept;
} // namespace lux::flowforge::detail
