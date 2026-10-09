#pragma once

#include <lux/engine/flowforge/FlowValueCompiler.hpp>

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
    [[nodiscard]] bool isUnsignedScalar(const meta::RefType&) noexcept;
    [[nodiscard]] bool isFloatingScalar(const meta::RefType&) noexcept;

    // Operand type validation/coercion stays with the original compiler boundary.
    // An operation of the wrong arity has no scalar instruction.
    [[nodiscard]] std::optional<EScalarInstruction>
    selectBinaryScalarInstruction(ENodeOperation, const meta::RefType&) noexcept;

    [[nodiscard]] std::optional<EScalarInstruction>
    selectUnaryScalarInstruction(ENodeOperation, const meta::RefType&) noexcept;
} // namespace lux::flowforge::detail
