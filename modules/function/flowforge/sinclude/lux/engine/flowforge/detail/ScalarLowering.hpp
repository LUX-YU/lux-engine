#pragma once

#include <lux/engine/flowforge/FlowValueCompiler.hpp>

#include <cstdint>
#include <optional>

namespace lux::meta
{
    struct RefType;
}

namespace lux::flowforge::detail
{
    enum class EScalarOperation : std::uint8_t
    {
        ADD,
        SUBTRACT,
        MULTIPLY,
        DIVIDE,
        MODULO,
        LOGICAL_AND,
        LOGICAL_OR,
        LOGICAL_NOT,
        NEGATE,
        CMP_EQ,
        CMP_NE,
        CMP_LT,
        CMP_LE,
        CMP_GT,
        CMP_GE,
    };

    [[nodiscard]] bool isUnsignedScalar(const meta::RefType&) noexcept;
    [[nodiscard]] bool isFloatingScalar(const meta::RefType&) noexcept;

    // Operand type validation/coercion stays with the original compiler boundary.
    // An operation of the wrong arity has no scalar instruction.
    [[nodiscard]] std::optional<EScalarInstruction>
    selectBinaryScalarInstruction(EScalarOperation, const meta::RefType&) noexcept;

    [[nodiscard]] std::optional<EScalarInstruction>
    selectUnaryScalarInstruction(EScalarOperation, const meta::RefType&) noexcept;
} // namespace lux::flowforge::detail
