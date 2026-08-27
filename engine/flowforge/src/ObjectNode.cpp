#include <lux/engine/flowforge/graph/ObjectNode.hpp>

namespace lux::flowforge {
    // ====================== GetObjectNode ======================
    /**
     * @brief Constructs a GetObjectNode with a generated ID.
     * @param info The RefType info for the object type.
     */
    GetObjectNode::GetObjectNode(const lux::meta::RefType &info)
        : GetObjectNode(reinterpret_cast<uintptr_t>(this), info) {}

    /**
     * @brief Constructs a GetObjectNode with a specified ID.
     * @param id The unique ID for this Node.
     * @param info The RefType info for the object type.
     */
    GetObjectNode::GetObjectNode(uint64_t id, const lux::meta::RefType &info)
        : Node(id, ENodeOperation::GET_OBJECT),
          data_out_pin_(this, DataPinInfo{"Value", &info}) {
      setName(info.name);
    }

    /**
     * @brief Retrieves the DataOutPin that outputs the read object.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin &GetObjectNode::dataOutPin() const { return data_out_pin_; }

    // ====================== SetObjectNode ======================

    /**
     * @brief Constructs a SetObjectNode with a generated ID.
     * @param info The RefType info for the object type.
     */
    SetObjectNode::SetObjectNode(const lux::meta::RefType &info)
        : SetObjectNode(reinterpret_cast<uintptr_t>(this), info) {}

    /**
     * @brief Constructs a SetObjectNode with a specified ID.
     * @param id The unique ID for this Node.
     * @param info The RefType info for the object type.
     */
    SetObjectNode::SetObjectNode(uint64_t id, const lux::meta::RefType &info)
        : ExecIntermediateNode(id, ENodeOperation::SET_OBJECT, "->", "Completed"),
          data_in_pin_(this, DataPinInfo{"Value", &info}),
          data_out_pin_(this, DataPinInfo{"Object Out", &info})
    {
        setName(info.name);
    }

    /**
     * @brief Retrieves the DataOutPin representing the updated object after
     * setting.
     * @return A constant reference to the DataOutPin.
     */
    const DataOutPin &SetObjectNode::dataOutPin() const { return data_out_pin_; }

    /**
     * @brief Retrieves the DataInPin representing the new object data to be set.
     * @return A constant reference to the DataInPin.
     */
    const DataInPin &SetObjectNode::dataInPin() const { return data_in_pin_; }

    // ====================== GetFieldNode ======================
    GetFieldNode::GetFieldNode(uint64_t id, const lux::meta::RefClass &cls,
                               const lux::meta::RefField &field)
        : Node(id, ENodeOperation::GET_FIELD),
          cls_(&cls),
          field_(&field),
          object_(this, DataPinInfo{"Object", &cls.type}),
          value_(this, DataPinInfo{std::string(field.name), &field.type})
    {
        // The object input is mandatory and cannot be represented as a wire
        // scalar constant. Keep it invalid until a producer is linked.
        object_.constantData() = lux::meta::RuntimeObject{};
        setName("Get " + std::string(cls.name) + "." + std::string(field.name));
    }

    GetFieldNode::GetFieldNode(const lux::meta::RefClass &cls,
                               const lux::meta::RefField &field)
        : GetFieldNode(reinterpret_cast<uintptr_t>(this), cls, field) {}

    // ====================== SetFieldNode ======================
    SetFieldNode::SetFieldNode(uint64_t id, const lux::meta::RefClass &cls,
                               const lux::meta::RefField &field)
        : ExecIntermediateNode(id, ENodeOperation::SET_FIELD),
          cls_(&cls),
          field_(&field),
          object_(this, DataPinInfo{"Object", &cls.type}),
          value_in_(this, DataPinInfo{std::string(field.name), &field.type}, /*allow_default=*/true),
          object_out_(this, DataPinInfo{"Object", &cls.type})
    {
        // See GetFieldNode: only the field value has a default payload.
        object_.constantData() = lux::meta::RuntimeObject{};
        setName("Set " + std::string(cls.name) + "." + std::string(field.name));
    }

    SetFieldNode::SetFieldNode(const lux::meta::RefClass &cls,
                               const lux::meta::RefField &field)
        : SetFieldNode(reinterpret_cast<uintptr_t>(this), cls, field) {}

    // ====================== GetVariableNode ======================
    GetVariableNode::GetVariableNode(uint64_t var_id, const DataPinInfo &info)
        : GetVariableNode(reinterpret_cast<uintptr_t>(this), var_id, info) {}

    GetVariableNode::GetVariableNode(uint64_t id, uint64_t var_id, const DataPinInfo &info)
        : Node(id, ENodeOperation::GET_VARIABLE),
          var_id_(var_id),
          value_(this, DataPinInfo{"Value", info.type})
    {
        setName("Get " + info.name);
    }

    // ====================== SetVariableNode ======================
    SetVariableNode::SetVariableNode(uint64_t var_id, const DataPinInfo &info)
        : SetVariableNode(reinterpret_cast<uintptr_t>(this), var_id, info) {}

    SetVariableNode::SetVariableNode(uint64_t id, uint64_t var_id, const DataPinInfo &info)
        : ExecIntermediateNode(id, ENodeOperation::SET_VARIABLE),
          var_id_(var_id),
          value_in_(this, DataPinInfo{"Value", info.type}, /*allow_default=*/true),
          value_out_(this, DataPinInfo{"Value", info.type})
    {
        setName("Set " + info.name);
    }
} // namespace lux::flowforge
