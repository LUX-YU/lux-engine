#pragma once
#include "NodeBase.hpp"

namespace lux::flowforge
{
    /**
     * @class GetObjectNode
     * @brief A node that reads an object's data (via reflection).
     */
     class GetObjectNode : public Node
     {
     public:
         /**
          * @brief Constructs a GetObjectNode with a generated ID.
          * @param type The runtime type info pointer for the object type.
          */
         GetObjectNode(const lux::meta::RefType& info);

         /**
          * @brief Constructs a GetObjectNode with a specified ID.
          * @param id The unique ID for this Node.
          * @param type The runtime type info pointer for the object type.
          */
         GetObjectNode(uint64_t id, const lux::meta::RefType& info);

         /**
          * @brief Gets the DataOutPin that represents the read object.
          * @return A const reference to the DataOutPin.
          */
         const DataOutPin& dataOutPin() const;

     private:
         DataOutPin data_out_pin_; ///< The data output pin that holds the read object reference.
     };

     /**
      * @class SetObjectNode
      * @brief A node that writes data to an object (via reflection).
      */
     class SetObjectNode : public ExecIntermediateNode
     {
     public:
         /**
          * @brief Constructs a SetObjectNode with a generated ID.
          * @param type The runtime type info pointer for the object type.
          */
         SetObjectNode(const lux::meta::RefType& info);

         /**
          * @brief Constructs a SetObjectNode with a specified ID.
          * @param id The unique ID for this Node.
          * @param type The runtime type info pointer for the object type.
          */
         SetObjectNode(uint64_t id, const lux::meta::RefType& info);

         /**
          * @brief Gets the DataOutPin that represents the updated object.
          * @return A const reference to the DataOutPin.
          */
         const DataOutPin& dataOutPin() const;

         /**
          * @brief Gets the DataInPin that represents the object data to be written.
          * @return A const reference to the DataInPin.
          */
         const DataInPin& dataInPin() const;

     private:
         DataInPin  data_in_pin_;  ///< Pin receiving the data to be set.
         DataOutPin data_out_pin_; ///< Pin outputting the updated object.
     };

    /**
     * @class GetFieldNode
     * @brief Reads a reflected field off an engine object. The graph never
     *        OWNS objects — the object pin carries an engine-provided
     *        pointer (event/function parameter).
     *
     * Pseudo-pure: no exec pins, but the field is re-read at EVERY use site
     * (never cached), so a SetField earlier on the exec chain is observed.
     */
    class GetFieldNode : public Node
    {
    public:
        GetFieldNode(uint64_t id, const lux::meta::RefClass& cls,
                     const lux::meta::RefField& field);
        GetFieldNode(const lux::meta::RefClass& cls,
                     const lux::meta::RefField& field);

        const lux::meta::RefClass* ownerClass() const { return cls_; }
        const lux::meta::RefField* field() const { return field_; }
        const DataInPin&  objectPin() const { return object_; }
        const DataOutPin& valuePin() const { return value_; }

    private:
        const lux::meta::RefClass* cls_;
        const lux::meta::RefField* field_;
        DataInPin  object_;
        DataOutPin value_;
    };

    /**
     * @class SetFieldNode
     * @brief Writes a reflected field on an engine object; the object
     *        reference passes through for convenient chaining.
     */
    class SetFieldNode : public ExecIntermediateNode
    {
    public:
        SetFieldNode(uint64_t id, const lux::meta::RefClass& cls,
                     const lux::meta::RefField& field);
        SetFieldNode(const lux::meta::RefClass& cls,
                     const lux::meta::RefField& field);

        const lux::meta::RefClass* ownerClass() const { return cls_; }
        const lux::meta::RefField* field() const { return field_; }
        const DataInPin&  objectPin() const { return object_; }
        const DataInPin&  valueIn() const { return value_in_; }
        const DataOutPin& objectOut() const { return object_out_; }

    private:
        const lux::meta::RefClass* cls_;
        const lux::meta::RefField* field_;
        DataInPin  object_;
        DataInPin  value_in_;
        DataOutPin object_out_;
    };

    /**
     * @class GetVariableNode
     * @brief Reads a graph-local variable (FlowGraph::GraphVariable) by id.
     *
     * Pseudo-pure: no exec pins, but it reads the variable's slot at EVERY
     * use site (the compiler never caches its value), so a Set earlier on
     * the exec chain is always observed.
     */
    class GetVariableNode : public Node
    {
    public:
        GetVariableNode(uint64_t var_id, const DataPinInfo& info);
        GetVariableNode(uint64_t id, uint64_t var_id, const DataPinInfo& info);

        uint64_t variableId() const { return var_id_; }
        const DataOutPin& valuePin() const { return value_; }

    private:
        uint64_t   var_id_;
        DataOutPin value_;
    };

    /**
     * @class SetVariableNode
     * @brief Writes a graph-local variable; passes the written value through
     *        on a data-out pin for convenient chaining.
     */
    class SetVariableNode : public ExecIntermediateNode
    {
    public:
        SetVariableNode(uint64_t var_id, const DataPinInfo& info);
        SetVariableNode(uint64_t id, uint64_t var_id, const DataPinInfo& info);

        uint64_t variableId() const { return var_id_; }
        const DataInPin&  valueIn() const { return value_in_; }
        const DataOutPin& valueOut() const { return value_out_; }

    private:
        uint64_t   var_id_;
        DataInPin  value_in_;
        DataOutPin value_out_;
    };
}
