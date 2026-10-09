#include "lux/engine/flowforge/graph/NodeBase.hpp"
#include <lux/engine/flowforge/graph/ControlNode.hpp>

namespace lux::flowforge
{
    // ====================== BranchNode ======================

    BranchNode::BranchNode()
        : ExecIntermediateNode(ENodeOperation::BRANCH, "->", "True", {"False"}),
          data_in_pin_(this, DataPinInfo{"Condition", &lux::meta::ref_type_of_v<bool>}, true)
    {
        setName("Branch");
        data_in_pin_.setConstantData(lux::meta::RuntimeObject(bool{false}));
    }

    const ExecOutPin& BranchNode::execOutPinUp() const
    {
        return execOutPin();
    }

    const ExecOutPin& BranchNode::execOutPinDown() const
    {
        return *extraOutPins()[0];
    }

    /**
     * @brief Retrieves the DataInPin that provides the boolean condition.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& BranchNode::dataInPin() const
    {
        return data_in_pin_;
    }

    StartNode::StartNode() : Node(ENodeOperation::START), THasExecOutPin("->")
    {
        setName("Start");
    }

    // ====================== SequenceNode ======================

    SequenceNode::SequenceNode(SequenceSchema schema) : ExecIntermediateNode(ENodeOperation::SEQUENCE, "->", "->")
    {
        setName("Sequence");
        exec_out_pins_.reserve(schema.additional_outputs);
        for (std::size_t index = 0; index != schema.additional_outputs; ++index)
        {
            exec_out_pins_.push_back(std::make_unique<ExecOutPin>(this));
        }
    }

    /**
     * @brief Retrieves the list of ExecOutPins belonging to this SequenceNode.
     * @return A constant reference to a vector of unique_ptr<ExecOutPin>.
     */
    const std::vector<std::unique_ptr<ExecOutPin>>& SequenceNode::execOutPins() const
    {
        return exec_out_pins_;
    }

    // ====================== ForLoopNode ======================

    // Index pins are int32 (UE ForLoop convention): the IV wires directly
    // into int32 arithmetic without a lossy conversion, which the pin
    // type-check would otherwise refuse. "Last Index" is EXCLUSIVE — the
    // loop runs [first, last).
    ForLoopNode::ForLoopNode()
        : ExecIntermediateNode(ENodeOperation::FOR_LOOP, "->", "loop body", {"Completed"}),
          first_index_(this, DataPinInfo{"First Index", &lux::meta::ref_type_of_v<int32_t>}, true),
          last_index_(this, DataPinInfo{"Last Index", &lux::meta::ref_type_of_v<int32_t>}, true),
          index_(this, DataPinInfo{"Index", &lux::meta::ref_type_of_v<int32_t>})
    {
        setName("For Loop");
        first_index_.setConstantData(lux::meta::RuntimeObject(int32_t{0}));
        last_index_.setConstantData(lux::meta::RuntimeObject(int32_t{10}));
    }

    /**
     * @brief Retrieves the ExecOutPin used for the loop body execution path.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& ForLoopNode::loopBody() const
    {
        return execOutPin();
    }

    /**
     * @brief Retrieves the ExecOutPin triggered when the loop completes.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& ForLoopNode::completed() const
    {
        return *extraOutPins()[0];
    }

    /**
     * @brief Retrieves the DataInPin representing the first iteration index.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& ForLoopNode::first_index() const
    {
        return first_index_;
    }

    /**
     * @brief Retrieves the DataInPin representing the last iteration index.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& ForLoopNode::lastIndex() const
    {
        return last_index_;
    }

    /**
     * @brief Retrieves the DataOutPin that holds the current loop index.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin& ForLoopNode::indexPin() const
    {
        return index_;
    }

    // ====================== WhileLoopNode ======================

    WhileLoopNode::WhileLoopNode()
        : ExecIntermediateNode(ENodeOperation::WHILE_LOOP, "->", "loop body", {"Completed"}),
          data_in_pin_(this, DataPinInfo{"Condition", &lux::meta::ref_type_of_v<bool>}, true)
    {
        setName("While Loop");
        data_in_pin_.setConstantData(lux::meta::RuntimeObject(bool{true}));
    }

    /**
     * @brief Retrieves the ExecOutPin for the loop body execution path.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& WhileLoopNode::loopBody() const
    {
        return execOutPin();
    }

    /**
     * @brief Retrieves the ExecOutPin triggered when the loop completes.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& WhileLoopNode::completed() const
    {
        return *extraOutPins()[0];
    }

    /**
     * @brief Retrieves the DataInPin representing the boolean loop condition.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& WhileLoopNode::dataInPin() const
    {
        return data_in_pin_;
    }

    ReturnNode::ReturnNode() : Node(ENodeOperation::RETURN), THasExecInPin("->")
    {
        setName("Return");
    }

    // ====================== BreakNode ======================

    BreakNode::BreakNode() : Node(ENodeOperation::BREAK), THasExecInPin("->")
    {
        setName("Break");
    }
} // namespace lux::flowforge
