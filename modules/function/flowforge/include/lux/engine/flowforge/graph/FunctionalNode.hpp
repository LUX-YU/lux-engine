#pragma once
#include "NodeBase.hpp"

namespace lux::flowforge
{
    struct FuncArgInfo
    {
        const lux::meta::RefType* type;
        std::string               name;
    };

    /**
     * @class FuncDefNode
     * @brief Entry node of a graph function. Owns the SIGNATURE (argument
     *        and return value lists); FuncReturnNode and GraphFuncCallNode
     *        derive their pins from it. Arguments surface as DataOutPins
     *        that the function body wires from; the MLIR lowering maps them
     *        to the generated func.func's entry-block arguments.
     */
	class FuncDefNode : public Node, public HasExecOutPin<FuncDefNode>
    {
    public:
        FuncDefNode(uint64_t id, std::string_view name,
                    std::vector<FuncArgInfo> args,
                    std::vector<FuncArgInfo> rets = {});
        FuncDefNode(std::string_view name,
                    std::vector<FuncArgInfo> args,
                    std::vector<FuncArgInfo> rets = {});

        const std::vector<FuncArgInfo>& argInfos() const { return args_; }
        const std::vector<FuncArgInfo>& retInfos() const { return rets_; }
        const std::vector<std::unique_ptr<DataOutPin>>& argPins() const { return arg_pins_; }

    private:
        std::vector<FuncArgInfo>                 args_;
        std::vector<FuncArgInfo>                 rets_;
        std::vector<std::unique_ptr<DataOutPin>> arg_pins_;
    };

    /**
     * @class FuncReturnNode
     * @brief Return point of a graph function; its data-in pins mirror the
     *        owning FuncDefNode's declared return values.
     */
    class FuncReturnNode : public Node, public HasExecInPin<FuncReturnNode>
    {
    public:
        FuncReturnNode(uint64_t id, const FuncDefNode& def);
        explicit FuncReturnNode(const FuncDefNode& def);

        const FuncDefNode* def() const { return def_; }
        const std::vector<std::unique_ptr<DataInPin>>& retPins() const { return ret_pins_; }

    private:
        const FuncDefNode*                      def_;
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
    class OnEventNode : public Node, public HasExecOutPin<OnEventNode>
    {
    public:
        OnEventNode(uint64_t id, std::string_view event_name,
                    std::vector<FuncArgInfo> params = {});
        explicit OnEventNode(std::string_view event_name,
                             std::vector<FuncArgInfo> params = {});

        const std::vector<FuncArgInfo>& paramInfos() const { return params_; }
        const std::vector<std::unique_ptr<DataOutPin>>& paramPins() const { return param_pins_; }

    private:
        std::vector<FuncArgInfo>                 params_;
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
        GraphFuncCallNode(uint64_t id, const FuncDefNode& callee);
        explicit GraphFuncCallNode(const FuncDefNode& callee);

        const FuncDefNode* callee() const { return callee_; }
        const std::vector<std::unique_ptr<DataInPin>>&  argPins() const { return arg_pins_; }
        const std::vector<std::unique_ptr<DataOutPin>>& resultPins() const { return result_pins_; }

    private:
        const FuncDefNode*                       callee_;
        std::vector<std::unique_ptr<DataInPin>>  arg_pins_;
        std::vector<std::unique_ptr<DataOutPin>> result_pins_;
    };

    /**
     * @class NativeFuncCall
     * @brief A node representing a call to a reflected function or method.
     */
    class NativeFuncCall : public ExecIntermediateNode
    {
    public:
        /**
         * @brief Constructs a NativeFuncCall for a free function with a given ID.
         * @param id The unique ID for this Node.
         * @param registry The meta registry used to find type information.
         * @param meta The function meta describing the function to call.
         */
        NativeFuncCall(uint64_t id, const lux::meta::RefFunction& ref_function);

        /**
         * @brief Constructs a NativeFuncCall for a class method with a given ID.
         * @param id The unique ID for this Node.
         * @param registry The meta registry used to find type information.
         * @param record_meta Unused in this constructor body, but indicates record context.
         * @param meta The method meta describing the class method to call.
         */
        NativeFuncCall(uint64_t id, const lux::meta::RefClass& ref_class, const lux::meta::RefMethod& ref_method);

        /**
         * @brief Constructs a NativeFuncCall for a free function with a generated ID.
         * @param registry The meta registry used to find type information.
         * @param meta The function meta describing the function to call.
         */
        NativeFuncCall(const lux::meta::RefFunction& meta);

        /**
         * @brief Constructs a NativeFuncCall for a class method with a generated ID.
         * @param registry The meta registry used to find type information.
         * @param record_meta Unused in this constructor body, but indicates record context.
         * @param meta The method meta describing the class method to call.
         */
        NativeFuncCall(const lux::meta::RefClass& ref_class, const lux::meta::RefMethod& ref_method);

        /**
         * @brief Gets the DataInPins representing the function or method parameters.
         * @return A const reference to a vector of unique_ptr to DataInPins.
         */
        const std::vector<std::unique_ptr<DataInPin>>& dataInPins() const;

        /**
         * @brief Gets the DataOutPin representing the function or method return value.
         * @return A const reference to the DataOutPin.
         */
        const DataOutPin& result() const;

        /**
         * @brief Gets the type info meta of the function or method.
         * @return A type_info_ptr_t representing the function or method.
         */
        const lux::meta::RefInvokable& info() const;

        /**
         * @brief Sets the user-defined name for this NativeFuncCall.
         * @param name The new name string.
         */
        void setName(std::string_view name);

        /**
         * @brief Re-targets this call node to a DIFFERENT reflected free function and
         *        rebuilds all parameter/result pins from its signature. Links on the
         *        rebuilt data pins are dropped (cleanly unlinked by the pin
         *        destructors); the caller (the editor's reconstruct path) re-validates
         *        and re-applies links that still fit the new pins. The fixed exec pins
         *        are untouched (their links survive). The node keeps its id; its
         *        display name follows the new function.
         */
        void rebind(const lux::meta::RefFunction& ref_function);

        /**
         * @brief Method flavor of rebind: re-targets to a reflected class method
         *        (rebuilds the parameter pins plus the leading "Self" pin).
         */
        void rebind(const lux::meta::RefClass& ref_class, const lux::meta::RefMethod& ref_method);

        /**
         * @brief Rebuilds the pins from the CURRENT invokable info. Call after the
         *        underlying reflected signature changed in place (e.g. a hot-reload
         *        re-registered the function with different parameters).
         */
        void reconstruct() override;

    private:
        /**
         * @brief Helper function to create input pins based on the function/method parameter types.
         * @param registry The meta registry for type lookup.
         * @param invokable_info The structure describing the invokable's parameters and return type.
         */
        void createPins(const std::vector<lux::meta::RefParam>& invokable_info);

        /**
         * @brief Drops + de-registers all parameter pins and the result pin, then
         *        rebuilds them from invokable_info (+ a leading "Self" pin when
         *        @p self_type is non-null). Shared by the constructors, rebind and
         *        reconstruct so there is exactly ONE pin-(re)build path.
         */
        void rebuildPins(const lux::meta::RefType* self_type);

        bool                                     is_method_;        ///< Indicates if this node calls a class method.
        const lux::meta::RefInvokable*           invokable_info;    ///< Meta info for the function/method being called.
        const lux::meta::RefType*                self_type_{ nullptr }; ///< Owning class type when is_method_ (drives the "Self" pin rebuild).
        std::vector<std::unique_ptr<DataInPin>>  data_in_pins_;      ///< Parameter pins.
        std::unique_ptr<DataOutPin>              result_;            ///< Return value pin (rebuilt on rebind/reconstruct).
    };
}
