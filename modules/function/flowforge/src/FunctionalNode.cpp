#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>

#include <exception>
#include <utility>

namespace lux::flowforge
{
    NativeFuncCall::NativeFuncCall(Definition definition) noexcept
        : ExecIntermediateNode(ENodeOperation::NATIVE_FUNC_CALL), definition_(std::move(definition))
    {
        if (!definition_)
        {
            std::terminate();
        }
        rebuildPins();
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

    /**
     * @brief Drops + de-registers all parameter pins and the result pin, then rebuilds
     *        them from the immutable definition. The ONE pin-(re)build path shared by the
     *        constructors, rebind and reconstruct, reproducing fresh-construction pin
     *        order: in_pins_ = [exec_in, params..., Self?], out_pins_ = [exec_out, result].
     */
    void NativeFuncCall::rebuildPins()
    {

        // De-register the old pins from the Node's pin lists BEFORE destroying them —
        // the Pin destructors only unlink links, they do not remove node-side entries.
        for (auto& pin : data_in_pins_)
        {
            removeInPin(pin.get());
        }
        data_in_pins_.clear();
        if (result_)
        {
            removeOutPin(result_.get());
            result_.reset();
        }

        result_ = std::make_unique<DataOutPin>(this, DataPinInfo{"Return", &info().return_type});
        createPins(info().parameters);
        if (const auto* self_type = ownerType())
        {
            auto self_pin = std::make_unique<DataInPin>(this, DataPinInfo{"Self", self_type});
            // Insert 'Self' pin at the beginning for convention
            data_in_pins_.insert(data_in_pins_.begin(), std::move(self_pin));
        }
    }

    void NativeFuncCall::rebind(Definition definition) noexcept
    {
        if (!definition)
        {
            std::terminate();
        }
        auto previous = std::exchange(definition_, std::move(definition));
        rebuildPins();
        setName(info().name);
    }

    const lux::meta::RefType* NativeFuncCall::ownerType() const noexcept
    {
        return definition_->receiver();
    }

    void NativeFuncCall::reconstruct()
    {
        rebuildPins();
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
    FuncReturnNode::FuncReturnNode(const FuncDefNode& def)
        : Node(ENodeOperation::FUNC_RETURN), THasExecInPin("->"), def_(&def)
    {
        setName("Return " + def.name());
        for (const auto& r : def.retInfos())
        {
            ret_pins_.push_back(std::make_unique<DataInPin>(this, DataPinInfo{r.name, r.type}, /*allow_default=*/true));
        }
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
    GraphFuncCallNode::GraphFuncCallNode(const FuncDefNode& callee)
        : ExecIntermediateNode(ENodeOperation::GRAPH_FUNC_CALL), callee_(&callee)
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

} // namespace lux::flowforge
