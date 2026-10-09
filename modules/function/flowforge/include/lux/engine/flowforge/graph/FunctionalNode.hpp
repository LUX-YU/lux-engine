#pragma once
#include "NodeBase.hpp"

namespace lux::flowforge
{
    struct FuncArgInfo
    {
        const lux::meta::RefType* type;
        std::string name;
    };

    /**
     * @class FuncDefNode
     * @brief Entry node of a graph function. Owns the SIGNATURE (argument
     *        and return value lists); FuncReturnNode and GraphFuncCallNode
     *        derive their pins from it. Arguments surface as DataOutPins
     *        that the function body wires from; the MLIR lowering maps them
     *        to the generated func.func's entry-block arguments.
     */
    class FuncDefNode : public Node, public THasExecOutPin<FuncDefNode>
    {
    public:
        FuncDefNode(std::string_view name, std::vector<FuncArgInfo> args, std::vector<FuncArgInfo> rets = {});

        const std::vector<FuncArgInfo>& argInfos() const
        {
            return args_;
        }

        const std::vector<FuncArgInfo>& retInfos() const
        {
            return rets_;
        }

        const std::vector<std::unique_ptr<DataOutPin>>& argPins() const
        {
            return arg_pins_;
        }

    private:
        std::vector<FuncArgInfo> args_;
        std::vector<FuncArgInfo> rets_;
        std::vector<std::unique_ptr<DataOutPin>> arg_pins_;
    };

    /**
     * @class FuncReturnNode
     * @brief Return point of a graph function; its data-in pins mirror the
     *        owning FuncDefNode's declared return values.
     */
    class FuncReturnNode : public Node, public THasExecInPin<FuncReturnNode>
    {
    public:
        // The definition is borrowed only while copying the pin schema. The stored reference is graph-local.
        FuncReturnNode(NodeId definition, const FuncDefNode& signature);

        [[nodiscard]] NodeId definitionId() const noexcept
        {
            return definition_;
        }

        [[nodiscard]] bool matchesSignature(const FuncDefNode&) const noexcept;
        [[nodiscard]] const FuncDefNode* resolveDefinition(const FlowGraph&) const noexcept;

        const std::vector<std::unique_ptr<DataInPin>>& retPins() const
        {
            return ret_pins_;
        }

    private:
        NodeId definition_;
        std::vector<std::unique_ptr<DataInPin>> ret_pins_;
    };

    /**
     * @class OnEventNode
     * @brief Named event entry point (BeginPlay / Tick / custom). Like
     *        FuncDefNode it surfaces its payload parameters as DataOutPins;
     *        the MLIR lowering emits one func.func per event (symbol
     *        `lux_event_<sanitized name>`) that the engine invokes through
     *        FlowScriptInstance. Events have no return values.
     */
    class OnEventNode : public Node, public THasExecOutPin<OnEventNode>
    {
    public:
        explicit OnEventNode(std::string_view event_name, std::vector<FuncArgInfo> params = {});

        const std::vector<FuncArgInfo>& paramInfos() const
        {
            return params_;
        }

        const std::vector<std::unique_ptr<DataOutPin>>& paramPins() const
        {
            return param_pins_;
        }

    private:
        std::vector<FuncArgInfo> params_;
        std::vector<std::unique_ptr<DataOutPin>> param_pins_;
    };

    /**
     * @class GraphFuncCallNode
     * @brief Calls a graph function defined by a FuncDefNode in the SAME
     *        graph. Pins are copied from the callee's signature; the MLIR
     *        lowering emits a plain func.call to the callee's func.func in
     *        the same module (whole-program), so recursion is legal.
     */
    class GraphFuncCallNode : public ExecIntermediateNode
    {
    public:
        // No definition address survives construction; use the receiving graph's NodeId.
        GraphFuncCallNode(NodeId callee, const FuncDefNode& signature);

        [[nodiscard]] NodeId calleeId() const noexcept
        {
            return callee_;
        }

        [[nodiscard]] bool matchesSignature(const FuncDefNode&) const noexcept;
        [[nodiscard]] const FuncDefNode* resolveCallee(const FlowGraph&) const noexcept;

        const std::vector<std::unique_ptr<DataInPin>>& argPins() const
        {
            return arg_pins_;
        }

        const std::vector<std::unique_ptr<DataOutPin>>& resultPins() const
        {
            return result_pins_;
        }

    private:
        NodeId callee_;
        std::vector<std::unique_ptr<DataInPin>> arg_pins_;
        std::vector<std::unique_ptr<DataOutPin>> result_pins_;
    };

    class NativeCallDefinition;

    // Node storage borrows pin types from an immutable, owned native signature.
    class NativeFuncCall : public ExecIntermediateNode
    {
    public:
        using Definition = std::shared_ptr<const NativeCallDefinition>;

        explicit NativeFuncCall(Definition) noexcept;

        [[nodiscard]] const std::vector<std::unique_ptr<DataInPin>>& dataInPins() const;
        [[nodiscard]] const DataOutPin& result() const;
        [[nodiscard]] const lux::meta::RefInvokable& info() const;
        [[nodiscard]] const lux::meta::RefType* ownerType() const noexcept;

        // Keeps node/exec identity; rebuilt data pins lose their links as before. The old definition
        // stays alive until every old pin and its default value has been destroyed.
        void rebind(Definition) noexcept;
        void reconstruct() override;

    private:
        void createPins(const std::vector<lux::meta::RefParam>&);
        void rebuildPins();

        Definition definition_;
        std::vector<std::unique_ptr<DataInPin>> data_in_pins_;
        std::unique_ptr<DataOutPin> result_;
    };
} // namespace lux::flowforge
