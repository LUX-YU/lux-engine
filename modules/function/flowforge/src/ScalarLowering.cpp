#include <lux/engine/flowforge/detail/ScalarLowering.hpp>
#include <lux/engine/flowforge/graph/NodeBase.hpp>

namespace lux::flowforge::detail
{
    bool isUnsignedScalar(const meta::RefType& type) noexcept
    {
        switch (static_cast<meta::EBaseType>(type.qtype.base))
        {
        case meta::EBaseType::UINT8:
        case meta::EBaseType::UINT16:
        case meta::EBaseType::UINT32:
        case meta::EBaseType::UINT64:
            return true;
        default:
            return false;
        }
    }

    bool isFloatingScalar(const meta::RefType& type) noexcept
    {
        const auto base = static_cast<meta::EBaseType>(type.qtype.base);
        return base == meta::EBaseType::FLOAT || base == meta::EBaseType::DOUBLE;
    }

    std::optional<EScalarInstruction> selectBinaryScalarInstruction(
        ENodeOperation operation,
        const meta::RefType& type
    ) noexcept
    {
        using I = EScalarInstruction;
        const bool is_float = isFloatingScalar(type);
        const bool is_unsigned = isUnsignedScalar(type);
        switch (operation)
        {
        case ENodeOperation::ADD:
            return is_float ? I::ADD_FLOAT : I::ADD_INTEGER;
        case ENodeOperation::SUBTRACT:
            return is_float ? I::SUBTRACT_FLOAT : I::SUBTRACT_INTEGER;
        case ENodeOperation::MULTIPLY:
            return is_float ? I::MULTIPLY_FLOAT : I::MULTIPLY_INTEGER;
        case ENodeOperation::DIVIDE:
            if (is_float)
            {
                return I::DIVIDE_FLOAT;
            }
            return is_unsigned ? I::DIVIDE_UNSIGNED : I::DIVIDE_SIGNED;
        case ENodeOperation::MODULO:
            if (is_float)
            {
                return I::REMAINDER_FLOAT;
            }
            return is_unsigned ? I::REMAINDER_UNSIGNED : I::REMAINDER_SIGNED;
        case ENodeOperation::LOGICAL_AND:
            return I::AND;
        case ENodeOperation::LOGICAL_OR:
            return I::OR;
        case ENodeOperation::CMP_EQ:
            return is_float ? I::EQUAL_ORDERED_FLOAT : I::EQUAL_INTEGER;
        case ENodeOperation::CMP_NE:
            return is_float ? I::NOT_EQUAL_ORDERED_FLOAT : I::NOT_EQUAL_INTEGER;
        case ENodeOperation::CMP_LT:
            if (is_float)
            {
                return I::LESS_ORDERED_FLOAT;
            }
            return is_unsigned ? I::LESS_UNSIGNED : I::LESS_SIGNED;
        case ENodeOperation::CMP_LE:
            if (is_float)
            {
                return I::LESS_EQUAL_ORDERED_FLOAT;
            }
            return is_unsigned ? I::LESS_EQUAL_UNSIGNED : I::LESS_EQUAL_SIGNED;
        case ENodeOperation::CMP_GT:
            if (is_float)
            {
                return I::GREATER_ORDERED_FLOAT;
            }
            return is_unsigned ? I::GREATER_UNSIGNED : I::GREATER_SIGNED;
        case ENodeOperation::CMP_GE:
            if (is_float)
            {
                return I::GREATER_EQUAL_ORDERED_FLOAT;
            }
            return is_unsigned ? I::GREATER_EQUAL_UNSIGNED : I::GREATER_EQUAL_SIGNED;
        default:
            return std::nullopt;
        }
    }

    std::optional<EScalarInstruction> selectUnaryScalarInstruction(
        ENodeOperation operation,
        const meta::RefType& type
    ) noexcept
    {
        switch (operation)
        {
        case ENodeOperation::NEGATE:
            return isFloatingScalar(type) ? EScalarInstruction::NEGATE_FLOAT : EScalarInstruction::NEGATE_INTEGER;
        case ENodeOperation::LOGICAL_NOT:
            return EScalarInstruction::NOT_BOOLEAN;
        default:
            return std::nullopt;
        }
    }
} // namespace lux::flowforge::detail
