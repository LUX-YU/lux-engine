#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>

namespace lux::flowforge
{
    namespace
    {
        const lux::meta::RefType* boolType()
        {
            return &lux::meta::ref_type_of_v<bool>;
        }

        // Comparisons and logical ops always produce bool; everything else
        // produces the operand type.
        const lux::meta::RefType* resultTypeFor(ENodeOperation op,
                                                const lux::meta::RefType* operand_type)
        {
            switch (op)
            {
            case ENodeOperation::CMP_EQ:
            case ENodeOperation::CMP_NE:
            case ENodeOperation::CMP_LT:
            case ENodeOperation::CMP_LE:
            case ENodeOperation::CMP_GT:
            case ENodeOperation::CMP_GE:
            case ENodeOperation::LOGICAL_AND:
            case ENodeOperation::LOGICAL_OR:
            case ENodeOperation::LOGICAL_NOT:
                return boolType();
            default:
                return operand_type;
            }
        }

        // Zero-value default constant for an unlinked operand pin, so a
        // freshly-dropped node compiles without wiring every input. Types
        // outside the scalar set get no default (the pin stays "no valid
        // constant" and the compiler reports it if left unlinked).
        lux::meta::RuntimeObject makeZeroConstant(const lux::meta::RefType* type)
        {
            using lux::meta::EBaseType;
            if (!type) return {};
            switch (static_cast<EBaseType>(type->qtype.base))
            {
            case EBaseType::Bool:   return lux::meta::RuntimeObject(bool{false});
            case EBaseType::Int8:   return lux::meta::RuntimeObject(int8_t{0});
            case EBaseType::Uint8:  return lux::meta::RuntimeObject(uint8_t{0});
            case EBaseType::Int16:  return lux::meta::RuntimeObject(int16_t{0});
            case EBaseType::Uint16: return lux::meta::RuntimeObject(uint16_t{0});
            case EBaseType::Int32:  return lux::meta::RuntimeObject(int32_t{0});
            case EBaseType::Uint32: return lux::meta::RuntimeObject(uint32_t{0});
            case EBaseType::Int64:  return lux::meta::RuntimeObject(int64_t{0});
            case EBaseType::Uint64: return lux::meta::RuntimeObject(uint64_t{0});
            case EBaseType::Float:  return lux::meta::RuntimeObject(float{0.0f});
            case EBaseType::Double: return lux::meta::RuntimeObject(double{0.0});
            default:                return {};
            }
        }
    } // namespace

    // ====================== BinaryOpNode ======================
    BinaryOpNode::BinaryOpNode(ENodeOperation op, const lux::meta::RefType* operand_type)
        : BinaryOpNode(reinterpret_cast<uintptr_t>(this), op, operand_type) {}

    BinaryOpNode::BinaryOpNode(uint64_t id, ENodeOperation op, const lux::meta::RefType* operand_type)
        : Node(id, op),
          operand_type_(operand_type),
          lhs_(this, DataPinInfo{"A", operand_type}, /*allow_default=*/true),
          rhs_(this, DataPinInfo{"B", operand_type}, /*allow_default=*/true),
          result_(this, DataPinInfo{"Result", resultTypeFor(op, operand_type)})
    {
        setName(toString(op));
        if (auto zero = makeZeroConstant(operand_type); zero.isValid())
        {
            lhs_.setConstantData(std::move(zero));
            rhs_.setConstantData(makeZeroConstant(operand_type));
        }
    }

    // ====================== UnaryOpNode ======================
    UnaryOpNode::UnaryOpNode(ENodeOperation op, const lux::meta::RefType* operand_type)
        : UnaryOpNode(reinterpret_cast<uintptr_t>(this), op, operand_type) {}

    UnaryOpNode::UnaryOpNode(uint64_t id, ENodeOperation op, const lux::meta::RefType* operand_type)
        : Node(id, op),
          operand_type_(operand_type),
          operand_(this, DataPinInfo{"A", operand_type}, /*allow_default=*/true),
          result_(this, DataPinInfo{"Result", resultTypeFor(op, operand_type)})
    {
        setName(toString(op));
        if (auto zero = makeZeroConstant(operand_type); zero.isValid())
            operand_.setConstantData(std::move(zero));
    }
} // namespace lux::flowforge
