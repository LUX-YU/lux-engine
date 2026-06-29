#include <lux/engine/flowforge/FunctionalNode.hpp>

namespace lux::flowforge
{
    // ====================== NativeFuncCall ======================
    /**
     * @brief Constructs a NativeFuncCall for a free function with a given ID.
     * @param id The unique ID for this Node.
     * @param ref_function Reference to the meta function information.
     */
    NativeFuncCall::NativeFuncCall(uint64_t id, const lux::meta::RefFunction& ref_function)
        : ExecIntermediateNode(id, ENodeOperation::NATIVE_FUNC_CALL)
        , is_method_(false)
        , invokable_info(&ref_function.invokable)
    {
        rebuildPins(nullptr);
        setName(ref_function.invokable.name);
    }

    /**
     * @brief Constructs a NativeFuncCall for a class method with a given ID.
     * @param id The unique ID for this Node.
     * @param ref_class Reference to the class metadata.
     * @param ref_method Reference to the method metadata.
     */
    NativeFuncCall::NativeFuncCall(uint64_t id,
        const lux::meta::RefClass& ref_class,
        const lux::meta::RefMethod& ref_method)
        : ExecIntermediateNode(id, ENodeOperation::NATIVE_FUNC_CALL)
        , is_method_(true)
        , invokable_info(&ref_method.invokable)
    {
        rebuildPins(&ref_class.type);
        setName(ref_method.invokable.name);
    }

    /**
     * @brief Constructs a NativeFuncCall for a free function with a generated ID.
     * @param ref_function Reference to the meta function information.
     */
    NativeFuncCall::NativeFuncCall(const lux::meta::RefFunction& ref_function)
        : NativeFuncCall(reinterpret_cast<uintptr_t>(this), ref_function)
    {
    }

    /**
     * @brief Constructs a NativeFuncCall for a class method with a generated ID.
     * @param ref_class Reference to the class metadata.
     * @param ref_method Reference to the method metadata.
     */
    NativeFuncCall::NativeFuncCall(const lux::meta::RefClass& ref_class, const lux::meta::RefMethod& ref_method)
        : NativeFuncCall(reinterpret_cast<uintptr_t>(this), ref_class, ref_method)
    {
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
                this, DataPinInfo{ std::string(param.name), &param.type },
                /*allow_default=*/true);
            data_in_pins_.push_back(std::move(new_pin));
        }
    }

    /**
     * @brief Drops + de-registers all parameter pins and the result pin, then rebuilds
     *        them from invokable_info. The ONE pin-(re)build path shared by the
     *        constructors, rebind and reconstruct, reproducing fresh-construction pin
     *        order: in_pins_ = [exec_in, params..., Self?], out_pins_ = [exec_out, result].
     */
    void NativeFuncCall::rebuildPins(const lux::meta::RefType* self_type)
    {
        self_type_ = self_type;

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

        result_ = std::make_unique<DataOutPin>(this, DataPinInfo{ "Return", &invokable_info->return_type });
        createPins(invokable_info->parameters);
        if (is_method_ && self_type_)
        {
            auto self_pin = std::make_unique<DataInPin>(this, DataPinInfo{ "Self", self_type_ });
            // Insert 'Self' pin at the beginning for convention
            data_in_pins_.insert(data_in_pins_.begin(), std::move(self_pin));
        }
    }

    void NativeFuncCall::rebind(const lux::meta::RefFunction& ref_function)
    {
        is_method_     = false;
        invokable_info = &ref_function.invokable;
        rebuildPins(nullptr);
        setName(ref_function.invokable.name);
    }

    void NativeFuncCall::rebind(const lux::meta::RefClass& ref_class, const lux::meta::RefMethod& ref_method)
    {
        is_method_     = true;
        invokable_info = &ref_method.invokable;
        rebuildPins(&ref_class.type);
        setName(ref_method.invokable.name);
    }

    void NativeFuncCall::reconstruct()
    {
        rebuildPins(self_type_);
    }

    /**
     * @brief Gets the type info meta of the function or method.
     * @return A reference to the function or method metadata.
     */
    const lux::meta::RefInvokable& NativeFuncCall::info() const
    {
        return *invokable_info;
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

    /**
     * @brief Sets the user-defined name for this NativeFuncCall.
     * @param name The new name for this node.
     */
    void NativeFuncCall::setName(std::string_view name)
    {
        Node::setName(name);
    }

    // ====================== FuncDefNode ======================
    FuncDefNode::FuncDefNode(uint64_t id, std::string_view name, std::vector<FuncArgInfo> arg)
        : Node(id, ENodeOperation::FUNC_DEF_START), HasExecOutPin("->")
    {
        setName(name);
        // TODO: create DataOutPins for function arguments based on 'arg'
    }

    FuncDefNode::FuncDefNode(std::string_view name, std::vector<FuncArgInfo> arg)
        : FuncDefNode(reinterpret_cast<uintptr_t>(this), name, std::move(arg))
    {
    }

}