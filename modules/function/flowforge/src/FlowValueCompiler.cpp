#include <lux/engine/flowforge/FlowValueCompiler.hpp>
#include <lux/engine/meta/Meta.hpp>

namespace lux::flowforge
{
    FlowForgeResult<FlowValue> FlowValueCompiler::emitScalar(
        EScalarInstruction instruction,
        std::span<const FlowValue> operands
    ) noexcept
    {
        using I = EScalarInstruction;
        const auto invalid = [](const char* message)
        { return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, message}); };
        const bool is_known = instruction >= I::ADD_INTEGER && instruction <= I::NOT_BOOLEAN;
        const bool is_unary = instruction >= I::NEGATE_INTEGER;
        const bool has_arity = operands.size() == (is_unary ? 1U : 2U);
        const bool is_invalid_instruction = !is_known || !has_arity;
        if (is_invalid_instruction)
        {
            return invalid("invalid scalar instruction or operand count");
        }
        const auto* operand_type = type(operands.front());
        if (!operand_type)
        {
            return invalid("scalar instruction references an unknown value");
        }
        for (const auto value : operands)
        {
            const auto* current = type(value);
            const bool is_matching = current && *current == *operand_type;
            if (!is_matching)
            {
                return invalid("scalar instruction operand types differ");
            }
        }
        using B = meta::EBaseType;
        const auto base = static_cast<B>(operand_type->qtype.base);
        const bool is_float = base == B::FLOAT || base == B::DOUBLE;
        const bool is_integer = base == B::BOOL || base == B::INT8 || base == B::UINT8 || base == B::INT16 ||
                                base == B::UINT16 || base == B::INT32 || base == B::UINT32 || base == B::INT64 ||
                                base == B::UINT64;
        const auto qualifier = static_cast<meta::ETypeQual>(operand_type->qtype.qual);
        const bool is_pointer = qualifier == meta::ETypeQual::PTR || qualifier == meta::ETypeQual::PTR_TO_CONST ||
                                qualifier == meta::ETypeQual::CONST_PTR ||
                                qualifier == meta::ETypeQual::CONST_PTR_TO_CONST;
        const bool expects_float =
            instruction == I::ADD_FLOAT || instruction == I::SUBTRACT_FLOAT || instruction == I::MULTIPLY_FLOAT ||
            instruction == I::DIVIDE_FLOAT || instruction == I::REMAINDER_FLOAT || instruction == I::NEGATE_FLOAT ||
            (instruction >= I::EQUAL_ORDERED_FLOAT && instruction <= I::GREATER_EQUAL_ORDERED_FLOAT);
        const bool is_type_mismatch = is_pointer || (expects_float ? !is_float : !is_integer) ||
                                      (instruction == I::NOT_BOOLEAN && base != B::BOOL);
        if (is_type_mismatch)
        {
            return invalid("scalar instruction does not accept this operand type");
        }
        const bool is_comparison = instruction >= I::EQUAL_INTEGER && instruction <= I::GREATER_EQUAL_ORDERED_FLOAT;
        const auto& result_type = is_comparison ? meta::ref_type_of_v<bool> : *operand_type;
        auto result = emitScalarImpl(instruction, operands, result_type);
        if (!result)
        {
            return result;
        }
        const auto* actual = type(*result);
        const bool is_result_mismatch = !actual || *actual != result_type;
        if (is_result_mismatch)
        {
            return invalid("scalar backend returned an invalid value type");
        }
        return result;
    }
} // namespace lux::flowforge
