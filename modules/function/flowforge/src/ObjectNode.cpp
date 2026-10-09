#include <lux/engine/flowforge/graph/ObjectNode.hpp>

namespace lux::flowforge
{
    // ====================== GetObjectNode ======================

    GetObjectNode::GetObjectNode(const lux::meta::RefType& info)
        : Node(ENodeOperation::GET_OBJECT), data_out_pin_(this, DataPinInfo{"Value", &info})
    {
        setName(info.name);
    }

    /**
     * @brief Retrieves the DataOutPin that outputs the read object.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin& GetObjectNode::dataOutPin() const
    {
        return data_out_pin_;
    }

    // ====================== SetObjectNode ======================

    SetObjectNode::SetObjectNode(const lux::meta::RefType& info)
        : ExecIntermediateNode(ENodeOperation::SET_OBJECT, "->", "Completed"),
          data_in_pin_(this, DataPinInfo{"Value", &info}), data_out_pin_(this, DataPinInfo{"Object Out", &info})
    {
        setName(info.name);
    }

    /**
     * @brief Retrieves the DataOutPin representing the updated object after
     * setting.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin& SetObjectNode::dataOutPin() const
    {
        return data_out_pin_;
    }

    /**
     * @brief Retrieves the DataInPin representing the new object data to be set.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin& SetObjectNode::dataInPin() const
    {
        return data_in_pin_;
    }

    // ====================== GetFieldNode ======================
    GetFieldNode::GetFieldNode(const lux::meta::RefClass& cls, const lux::meta::RefField& field)
        : Node(ENodeOperation::GET_FIELD), cls_(&cls), field_(&field), object_(this, DataPinInfo{"Object", &cls.type}),
          value_(this, DataPinInfo{std::string(field.name), &field.type})
    {
        // The object input is mandatory and cannot be represented as a wire
        // scalar constant. Keep it invalid until a producer is linked.
        object_.constantData() = lux::meta::RuntimeObject{};
        setName("Get " + std::string(cls.name) + "." + std::string(field.name));
    }

    // ====================== SetFieldNode ======================
    SetFieldNode::SetFieldNode(const lux::meta::RefClass& cls, const lux::meta::RefField& field)
        : ExecIntermediateNode(ENodeOperation::SET_FIELD), cls_(&cls), field_(&field),
          object_(this, DataPinInfo{"Object", &cls.type}),
          value_in_(this, DataPinInfo{std::string(field.name), &field.type}, /*allow_default=*/true),
          object_out_(this, DataPinInfo{"Object", &cls.type})
    {
        // See GetFieldNode: only the field value has a default payload.
        object_.constantData() = lux::meta::RuntimeObject{};
        setName("Set " + std::string(cls.name) + "." + std::string(field.name));
    }

    // ====================== GetVariableNode ======================

    GetVariableNode::GetVariableNode(uint64_t var_id, const DataPinInfo& info)
        : Node(ENodeOperation::GET_VARIABLE), var_id_(var_id), value_(this, DataPinInfo{"Value", info.type})
    {
        setName("Get " + info.name);
    }

    // ====================== SetVariableNode ======================

    SetVariableNode::SetVariableNode(uint64_t var_id, const DataPinInfo& info)
        : ExecIntermediateNode(ENodeOperation::SET_VARIABLE), var_id_(var_id),
          value_in_(this, DataPinInfo{"Value", info.type}, /*allow_default=*/true),
          value_out_(this, DataPinInfo{"Value", info.type})
    {
        setName("Set " + info.name);
    }
} // namespace lux::flowforge
