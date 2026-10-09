#pragma once

#include "NodeBase.hpp"

namespace lux::flowforge
{
    /**
     * @class StartNode
     * @brief A special node representing the entry point of a flow graph.
     */
    class StartNode : public Node, public THasExecOutPin<StartNode>
    {
    public:
        StartNode();
    };

    /**
     * @class BranchNode
     * @brief A node that branches execution based on a boolean input.
     */
    class BranchNode : public ExecIntermediateNode
    {
    public:
        BranchNode();

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
        DataInPin data_in_pin_; ///< The boolean condition input pin.
    };

    struct SequenceSchema final
    {
        std::size_t additional_outputs{};
    };

    /**
     * @class SequenceNode
     * @brief A node that sequences multiple ExecOutPins from a single ExecInPin.
     */
    class SequenceNode : public ExecIntermediateNode
    {
    public:
        // Complete schema is constructed off graph; replace it through FlowGraphEdit.
        explicit SequenceNode(SequenceSchema schema = {});

        /**
         * @brief Gets the list of ExecOutPins for this SequenceNode.
         * @return A const reference to a vector of unique_ptr to ExecOutPins.
         */
        const std::vector<std::unique_ptr<ExecOutPin>>& execOutPins() const;

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
        ForLoopNode();

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
        DataInPin first_index_; ///< The input pin specifying the start index.
        DataInPin last_index_;  ///< The input pin specifying the end index.
        DataOutPin index_;      ///< The output pin exposing the current loop index.
    };

    /**
     * @class WhileLoopNode
     * @brief A node representing a while-loop with a boolean condition.
     */
    class WhileLoopNode : public ExecIntermediateNode
    {
    public:
        WhileLoopNode();

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
        DataInPin data_in_pin_; ///< The boolean condition input pin.
    };

    class ReturnNode : public Node, public THasExecInPin<ReturnNode>
    {
    public:
        ReturnNode();

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
    class BreakNode : public Node, public THasExecInPin<BreakNode>
    {
    public:
        BreakNode();
    };
} // namespace lux::flowforge
