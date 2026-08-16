#pragma once
#include "NodeBase.hpp"

namespace lux::flowforge
{
    /**
     * @class BinaryOpNode
     * @brief One class for every two-operand PURE data node (arithmetic,
     *        comparison, logical and/or). The concrete operation is the
     *        ENodeOperation tag; both operands share one declared type,
     *        chosen at creation (the palette registers per-type variants).
     *
     * Pure: no exec pins. The MLIR lowering re-evaluates the node on demand
     * at every use site (per region), so its value always reflects the
     * current values of its inputs.
     */
    class BinaryOpNode : public Node
    {
    public:
        /**
         * @brief Constructs a BinaryOpNode, using its own pointer as ID.
         * @param op            The operation tag (ADD..MODULO, LOGICAL_AND/OR, CMP_*).
         * @param operand_type  Declared type of both operands (comparisons and
         *                      logical ops still produce bool regardless).
         */
        BinaryOpNode(ENodeOperation op, const lux::meta::RefType* operand_type);
        BinaryOpNode(uint64_t id, ENodeOperation op, const lux::meta::RefType* operand_type);

        const DataInPin&  lhs() const { return lhs_; }
        const DataInPin&  rhs() const { return rhs_; }
        const DataOutPin& result() const { return result_; }

        const lux::meta::RefType* operandType() const { return operand_type_; }

    private:
        const lux::meta::RefType* operand_type_;
        DataInPin  lhs_;
        DataInPin  rhs_;
        DataOutPin result_;
    };

    /**
     * @class UnaryOpNode
     * @brief One-operand PURE data node: NEGATE and LOGICAL_NOT.
     */
    class UnaryOpNode : public Node
    {
    public:
        UnaryOpNode(ENodeOperation op, const lux::meta::RefType* operand_type);
        UnaryOpNode(uint64_t id, ENodeOperation op, const lux::meta::RefType* operand_type);

        const DataInPin&  operand() const { return operand_; }
        const DataOutPin& result() const { return result_; }

        const lux::meta::RefType* operandType() const { return operand_type_; }

    private:
        const lux::meta::RefType* operand_type_;
        DataInPin  operand_;
        DataOutPin result_;
    };
} // namespace lux::flowforge
