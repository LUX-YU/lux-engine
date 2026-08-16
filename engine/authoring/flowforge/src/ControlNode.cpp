#include "lux/engine/authoring/flowforge/NodeBase.hpp"
#include <lux/engine/authoring/flowforge/ControlNode.hpp>

namespace lux::flowforge
{
    // ====================== BranchNode ======================
    /**
     * @brief Default constructor for a BranchNode, uses its own pointer as ID.
     */
    BranchNode::BranchNode() : BranchNode(reinterpret_cast<uintptr_t>(this)) {}

    /**
     * @brief Constructs a BranchNode with a specified ID.
     * @param id The unique ID for this Node.
     */
    BranchNode::BranchNode(uint64_t id)
        : ExecIntermediateNode(id, ENodeOperation::BRANCH, "->", "True", { "False" }),
        data_in_pin_(this, DataPinInfo{ "Condition", &lux::meta::ref_type_of_v<bool> }, true)
    {
        setName("Branch");
        data_in_pin_.setConstantData(lux::meta::RuntimeObject(bool{ false }));
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
    const DataInPin& BranchNode::dataInPin() const { return data_in_pin_; }

    /**
     * @brief Default constructor for a StartNode, sets the Node ID to the pointer of this object.
     */
     StartNode::StartNode() 
         : StartNode(reinterpret_cast<uintptr_t>(this)) { }

     /**
      * @brief Constructs a StartNode with a specified ID.
      * @param id The unique ID for this Node.
      */
     StartNode::StartNode(uint64_t id) 
         : Node(id, ENodeOperation::START), HasExecOutPin("->")
     {
         setName("Start");
     }

    // ====================== SequenceNode ======================

    /**
     * @brief Default constructor for a SequenceNode, uses its own pointer as ID.
     */
    SequenceNode::SequenceNode() : SequenceNode(reinterpret_cast<uintptr_t>(this)) {}

    /**
     * @brief Constructs a SequenceNode with a specified ID.
     * @param id The unique ID for this Node.
     */
    SequenceNode::SequenceNode(uint64_t id)
        : ExecIntermediateNode(id, ENodeOperation::SEQUENCE, "->", "->")
    {
        setName("Sequence");
    }

    /**
     * @brief Retrieves the list of ExecOutPins belonging to this SequenceNode.
     * @return A constant reference to a vector of unique_ptr<ExecOutPin>.
     */
    const std::vector<std::unique_ptr<ExecOutPin>>& SequenceNode::execOutPins() const
    {
        return exec_out_pins_;
    }

    /**
     * @brief Adds an ExecOutPin to this SequenceNode.
     */
    void SequenceNode::addExecOutPin()
    {
        exec_out_pins_.emplace_back(std::make_unique<ExecOutPin>(this));
    }

    /**
     * @brief Removes the last ExecOutPin from this SequenceNode.
     */
    void SequenceNode::removeExecOutPin()
    {
        removeOutPin(exec_out_pins_.back().get());
        exec_out_pins_.pop_back();
    }

    // ====================== ForLoopNode ======================

    /**
     * @brief Default constructor for a ForLoopNode, uses its own pointer as ID.
     */
    ForLoopNode::ForLoopNode() : ForLoopNode(reinterpret_cast<uintptr_t>(this)) {}

    /**
     * @brief Constructs a ForLoopNode with a specified ID.
     * @param id The unique ID for this Node.
     */
    // Index pins are int32 (UE ForLoop convention): the IV wires directly
    // into int32 arithmetic without a lossy conversion, which the pin
    // type-check would otherwise refuse. "Last Index" is EXCLUSIVE — the
    // loop runs [first, last).
    ForLoopNode::ForLoopNode(uint64_t id)
        : ExecIntermediateNode(id, ENodeOperation::FOR_LOOP, "->", "loop body", {"Completed"}),
        first_index_(this, DataPinInfo{ "First Index", &lux::meta::ref_type_of_v<int32_t> }, true),
        last_index_(this, DataPinInfo{ "Last Index", &lux::meta::ref_type_of_v<int32_t> }, true),
        index_(this, DataPinInfo{ "Index", &lux::meta::ref_type_of_v<int32_t> })
    {
        setName("For Loop");
        first_index_.setConstantData(lux::meta::RuntimeObject(int32_t{0}));
        last_index_.setConstantData(lux::meta::RuntimeObject(int32_t{10}));
    }

    /**
     * @brief Retrieves the ExecOutPin used for the loop body execution path.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& ForLoopNode::loopBody() const { return execOutPin(); }

    /**
     * @brief Retrieves the ExecOutPin triggered when the loop completes.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& ForLoopNode::completed() const { return *extraOutPins()[0]; }

    /**
     * @brief Retrieves the DataInPin representing the first iteration index.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& ForLoopNode::first_index() const { return first_index_; }

    /**
     * @brief Retrieves the DataInPin representing the last iteration index.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& ForLoopNode::lastIndex() const { return last_index_; }

    /**
     * @brief Retrieves the DataOutPin that holds the current loop index.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin& ForLoopNode::indexPin() const { return index_; }

    // ====================== WhileLoopNode ======================

    /**
     * @brief Default constructor for a WhileLoopNode, uses its own pointer as ID.
     */
    WhileLoopNode::WhileLoopNode() : WhileLoopNode(reinterpret_cast<uintptr_t>(this)) {}

    /**
     * @brief Constructs a WhileLoopNode with a specified ID.
     * @param id The unique ID for this Node.
     */
    WhileLoopNode::WhileLoopNode(uint64_t id)
        : ExecIntermediateNode(id, ENodeOperation::WHILE_LOOP, "->", "loop body", { "Completed" }),
        data_in_pin_(this, DataPinInfo{ "Condition", &lux::meta::ref_type_of_v<bool> }, true)
    {
        setName("While Loop");
        data_in_pin_.setConstantData(lux::meta::RuntimeObject(bool{ true }));
    }

    /**
     * @brief Retrieves the ExecOutPin for the loop body execution path.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& WhileLoopNode::loopBody() const { return execOutPin(); }

    /**
     * @brief Retrieves the ExecOutPin triggered when the loop completes.
     * @return A constant reference to the ExecOutPin.
     */
    const ExecOutPin& WhileLoopNode::completed() const { return *extraOutPins()[0]; }

    /**
     * @brief Retrieves the DataInPin representing the boolean loop condition.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& WhileLoopNode::dataInPin() const { return data_in_pin_; }


    ReturnNode::ReturnNode()
        : ReturnNode(reinterpret_cast<uintptr_t>(this)){}

    /**
     * @brief Constructs a ReturnNode with a specified ID.
     * @param id The unique ID for this Node.
     * @param type The runtime type info pointer for the return value type.
     */
    ReturnNode::ReturnNode(uint64_t id)
        : Node(id, ENodeOperation::RETURN), HasExecInPin("->")
    {
		setName("Return");
    }

    // ====================== BreakNode ======================
    BreakNode::BreakNode()
        : BreakNode(reinterpret_cast<uintptr_t>(this)) {}

    BreakNode::BreakNode(uint64_t id)
        : Node(id, ENodeOperation::BREAK), HasExecInPin("->")
    {
        setName("Break");
    }
}
