#include "NodeBase.hpp"

namespace lux::flowforge
{
    /**
     * @class StartNode
     * @brief A special node representing the entry point of a flow graph.
     */
    class StartNode :  public Node, public HasExecOutPin<StartNode>
    {
    public:
        /**
         * @brief Default constructor, initializes the node with a generated ID.
         */
        StartNode();

        /**
         * @brief Constructs a StartNode with a specified ID.
         * @param id The unique ID for this Node.
         */
        StartNode(uint64_t id);
    };

    /**
     * @class BranchNode
     * @brief A node that branches execution based on a boolean input.
     */
    class BranchNode : public ExecIntermediateNode
    {
    public:
        /**
         * @brief Default constructor, creates a BranchNode with a generated ID.
         */
        BranchNode();

        /**
         * @brief Constructs a BranchNode with a given ID.
         * @param id The unique ID for this Node.
         */
        BranchNode(uint64_t id);

        /**
         * @brief Gets the 'true' branch ExecOutPin.
         * @return A const reference to the 'false' ExecOutPin.
         */
        const ExecOutPin& execOutPinUp() const;

        /**
         * @brief Gets the 'false' branch ExecOutPin.
         * @return A const reference to the 'false' ExecOutPin.
         */
        const ExecOutPin& execOutPinDown() const;

        /**
         * @brief Gets the DataInPin representing the boolean condition.
         * @return A const reference to the DataInPin.
         */
        const DataInPin& dataInPin() const;

    private:
        DataInPin  data_in_pin_;       ///< The boolean condition input pin.
    };

    /**
     * @class SequenceNode
     * @brief A node that sequences multiple ExecOutPins from a single ExecInPin.
     */
    class SequenceNode : public ExecIntermediateNode
    {
    public:
        /**
         * @brief Default constructor, creates a SequenceNode with a generated ID.
         */
        SequenceNode();

        /**
         * @brief Constructs a SequenceNode with a given ID.
         * @param id The unique ID for this Node.
         */
        SequenceNode(uint64_t id);

        /**
         * @brief Gets the list of ExecOutPins for this SequenceNode.
         * @return A const reference to a vector of unique_ptr to ExecOutPins.
         */
        const std::vector<std::unique_ptr<ExecOutPin>>& execOutPins() const;

        /**
         * @brief Adds a new ExecOutPin to this SequenceNode.
         */
        [[nodiscard]] ExecOutPin* addExecOutPin(PinId stable_id = {});

        /**
         * @brief Removes the last ExecOutPin from this SequenceNode.
         */
        [[nodiscard]] PinId removeExecOutPin();

		using ExecIntermediateNode::addExecOutPin;
		using ExecIntermediateNode::removeExecOutPin;

    private:
        std::vector<std::unique_ptr<ExecOutPin>> exec_out_pins_; ///< The executable output pins.
    };

    /**
     * @class ForLoopNode
     * @brief A node representing a for-loop with integer iteration.
     */
    class ForLoopNode : public ExecIntermediateNode
    {
    public:
        /**
         * @brief Default constructor, creates a ForLoopNode with a generated ID.
         */
        ForLoopNode();

        /**
         * @brief Constructs a ForLoopNode with a given ID.
         * @param id The unique ID for this Node.
         */
        ForLoopNode(uint64_t id);

        /**
         * @brief Gets the ExecOutPin for the loop body execution.
         * @return A const reference to the ExecOutPin.
         */
        const ExecOutPin& loopBody() const;

        /**
         * @brief Gets the ExecOutPin for when the loop completes.
         * @return A const reference to the ExecOutPin.
         */
        const ExecOutPin& completed() const;

        /**
         * @brief Gets the DataInPin that specifies the first index of the loop.
         * @return A const reference to the DataInPin.
         */
        const DataInPin& first_index() const;

        /**
         * @brief Gets the DataInPin that specifies the last index of the loop.
         * @return A const reference to the DataInPin.
         */
        const DataInPin& lastIndex() const;

        /**
         * @brief Gets the DataOutPin that holds the current loop index.
         * @return A const reference to the DataOutPin.
         */
        const DataOutPin& indexPin() const;

    private:
        DataInPin  first_index_; ///< The input pin specifying the start index.
        DataInPin  last_index_;  ///< The input pin specifying the end index.
        DataOutPin index_;       ///< The output pin exposing the current loop index.
    };

    /**
     * @class WhileLoopNode
     * @brief A node representing a while-loop with a boolean condition.
     */
    class WhileLoopNode : public ExecIntermediateNode
    {
    public:
        /**
         * @brief Default constructor, creates a WhileLoopNode with a generated ID.
         */
        WhileLoopNode();

        /**
         * @brief Constructs a WhileLoopNode with a given ID.
         * @param id The unique ID for this Node.
         */
        WhileLoopNode(uint64_t id);

        /**
         * @brief Gets the ExecOutPin for the loop body execution.
         * @return A const reference to the ExecOutPin.
         */
        const ExecOutPin& loopBody() const;

        /**
         * @brief Gets the ExecOutPin for when the loop completes.
         * @return A const reference to the ExecOutPin.
         */
        const ExecOutPin& completed() const;

        /**
         * @brief Gets the DataInPin that specifies the boolean condition.
         * @return A const reference to the DataInPin.
         */
        const DataInPin& dataInPin() const;

    private:
        DataInPin  data_in_pin_; ///< The boolean condition input pin.
    };

	class ReturnNode : public Node, public HasExecInPin<ReturnNode>
    {
    public:
        ReturnNode();

        /**
         * @brief Constructs a ReturnNode with a specified ID.
         * @param id The unique ID for this Node.
         * @param type The runtime type info pointer for the return value type.
         */
        ReturnNode(uint64_t id);

        /**
         * @brief Gets the DataInPin that represents the return value to be returned.
         * @return A const reference to the DataInPin.
         */
        const DataInPin& dataInPin() const;
    };

    /**
     * @class BreakNode
     * @brief Exits the innermost enclosing loop. Only valid on an exec chain
     *        nested inside a ForLoop/WhileLoop body — the MLIR builder
     *        rejects a Break outside any loop at compile time.
     */
    class BreakNode : public Node, public HasExecInPin<BreakNode>
    {
    public:
        BreakNode();
        BreakNode(uint64_t id);
    };
}
