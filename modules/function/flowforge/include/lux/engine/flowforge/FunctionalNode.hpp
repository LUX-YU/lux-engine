#pragma once
#include "NodeBase.hpp"

namespace lux::flowforge
{
    struct FuncArgInfo
    {
        lux::meta::RefType* type;
        std::string         name;
    };

	class LUX_FUNCTION_PUBLIC FuncDefNode : public Node, public HasExecOutPin<FuncDefNode>
    {
    public:
        FuncDefNode(uint64_t id, std::string_view name, std::vector<FuncArgInfo> arg);

        FuncDefNode(std::string_view name, std::vector<FuncArgInfo> arg);
    };

    /**
     * @class NativeFuncCall
     * @brief A node representing a call to a reflected function or method.
     */
    class LUX_FUNCTION_PUBLIC NativeFuncCall : public ExecIntermediateNode
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
