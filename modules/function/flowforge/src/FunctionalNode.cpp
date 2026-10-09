#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>

#include <exception>
#include <utility>

namespace lux::flowforge
{
    namespace
    {
        template <class PinType>
        [[nodiscard]] bool matchesPins(
            const std::vector<std::unique_ptr<PinType>>& pins,
            std::span<const FuncArgInfo> signature
        ) noexcept
        {
            if (pins.size() != signature.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < pins.size(); ++i)
            {
                const auto* actual = pins[i]->info().type;
                const auto* expected = signature[i].type;
                const bool has_types = actual != nullptr && expected != nullptr;
                if (!has_types || *actual != *expected)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] const FuncDefNode* findDefinition(const FlowGraph& graph, NodeId id) noexcept
        {
            const auto* node = graph.findNodeById(id);
            const bool is_definition = node != nullptr && node->operation() == ENodeOperation::FUNC_DEF_START;
            return is_definition ? static_cast<const FuncDefNode*>(node) : nullptr;
        }
    } // namespace

    NativeFuncCall::NativeFuncCall(Definition definition) noexcept
        : ExecIntermediateNode(ENodeOperation::NATIVE_FUNC_CALL), definition_(std::move(definition))
    {
        if (!definition_)
        {
            std::terminate();
        }
        buildPins();
        setName(info().name);
    }

    /**
     * @brief Helper function to create input pins based on the function/method parameter types.
     * @param params The parameters information from the function or method.
     */
    void NativeFuncCall::createPins(const std::vector<lux::meta::RefParam>& params)
    {
        for (const auto& param : params)
        {
            // Create a new input pin for each parameter. Parameters ALLOW a
            // default: an unconnected pin supplies its (editor-editable)
            // constant as the argument — the MLIR lowering's constant path
            // requires allowDefault() (the Self pin of methods deliberately
            // does not allow one).
            auto new_pin = std::make_unique<DataInPin>(
                this,
                DataPinInfo{std::string(param.name), &param.type},
                /*allow_default=*/true
            );
            data_in_pins_.push_back(std::move(new_pin));
        }
    }

    // Build the complete detached schema once. Replacement is a FlowGraphEdit transaction.
    void NativeFuncCall::buildPins()
    {
        result_ = std::make_unique<DataOutPin>(this, DataPinInfo{"Return", &info().return_type});
        createPins(info().parameters);
        if (const auto* self_type = ownerType())
        {
            auto self_pin = std::make_unique<DataInPin>(this, DataPinInfo{"Self", self_type});
            // Insert 'Self' pin at the beginning for convention
            data_in_pins_.insert(data_in_pins_.begin(), std::move(self_pin));
        }
    }

    const lux::meta::RefType* NativeFuncCall::ownerType() const noexcept
    {
        return definition_->receiver();
    }

    /**
     * @brief Gets the type info meta of the function or method.
     * @return A reference to the function or method metadata.
     */
    const lux::meta::RefInvokable& NativeFuncCall::info() const
    {
        return definition_->signature();
    }

    /**
     * @brief Retrieves all DataInPins for this NativeFuncCall (represents parameters).
     * @return A constant reference to a vector of unique_ptr<DataInPin>.
     */
    const std::vector<std::unique_ptr<DataInPin>>& NativeFuncCall::dataInPins() const
    {
        return data_in_pins_;
    }

    /**
     * @brief Retrieves the DataOutPin representing the function/method return value.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin& NativeFuncCall::result() const
    {
        return *result_;
    }

    // ====================== FuncDefNode ======================
    FuncDefNode::FuncDefNode(std::string_view name, std::vector<FuncArgInfo> args, std::vector<FuncArgInfo> rets)
        : Node(ENodeOperation::FUNC_DEF_START), THasExecOutPin("->"), args_(std::move(args)), rets_(std::move(rets))
    {
        setName(name);
        for (const auto& a : args_)
        {
            arg_pins_.push_back(std::make_unique<DataOutPin>(this, DataPinInfo{a.name, a.type}));
        }
    }

    // ====================== FuncReturnNode ======================
    FuncReturnNode::FuncReturnNode(NodeId definition, const FuncDefNode& def)
        : Node(ENodeOperation::FUNC_RETURN), THasExecInPin("->"), definition_(definition)
    {
        setName("Return " + def.name());
        for (const auto& r : def.retInfos())
        {
            ret_pins_.push_back(std::make_unique<DataInPin>(this, DataPinInfo{r.name, r.type}, /*allow_default=*/true));
        }
    }

    bool FuncReturnNode::matchesSignature(const FuncDefNode& definition) const noexcept
    {
        return matchesPins(ret_pins_, definition.retInfos());
    }

    const FuncDefNode* FuncReturnNode::resolveDefinition(const FlowGraph& graph) const noexcept
    {
        const auto* definition = findDefinition(graph, definition_);
        return definition && matchesSignature(*definition) ? definition : nullptr;
    }

    // ====================== OnEventNode ======================
    OnEventNode::OnEventNode(std::string_view event_name, std::vector<FuncArgInfo> params)
        : Node(ENodeOperation::ON_EVENT), THasExecOutPin("->"), params_(std::move(params))
    {
        setName(event_name);
        for (const auto& p : params_)
        {
            param_pins_.push_back(std::make_unique<DataOutPin>(this, DataPinInfo{p.name, p.type}));
        }
    }

    // ====================== GraphFuncCallNode ======================
    GraphFuncCallNode::GraphFuncCallNode(NodeId callee_id, const FuncDefNode& callee)
        : ExecIntermediateNode(ENodeOperation::GRAPH_FUNC_CALL), callee_(callee_id)
    {
        setName("Call " + callee.name());
        for (const auto& a : callee.argInfos())
        {
            arg_pins_.push_back(std::make_unique<DataInPin>(this, DataPinInfo{a.name, a.type}, /*allow_default=*/true));
        }
        for (const auto& r : callee.retInfos())
        {
            result_pins_.push_back(std::make_unique<DataOutPin>(this, DataPinInfo{r.name, r.type}));
        }
    }

    bool GraphFuncCallNode::matchesSignature(const FuncDefNode& definition) const noexcept
    {
        return matchesPins(arg_pins_, definition.argInfos()) && matchesPins(result_pins_, definition.retInfos());
    }

    const FuncDefNode* GraphFuncCallNode::resolveCallee(const FlowGraph& graph) const noexcept
    {
        const auto* definition = findDefinition(graph, callee_);
        return definition && matchesSignature(*definition) ? definition : nullptr;
    }

} // namespace lux::flowforge
