#include <lux/engine/flowforge/ObjectNode.hpp>

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
} // namespace lux::flowforge