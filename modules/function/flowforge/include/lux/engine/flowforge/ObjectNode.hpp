#pragma once
#include "NodeBase.hpp"

namespace lux::flowforge
{
    /**
     * @class GetObjectNode
     * @brief A node that reads an object's data (via reflection).
     */
     class LUX_FUNCTION_PUBLIC GetObjectNode : public Node
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
     class LUX_FUNCTION_PUBLIC SetObjectNode : public ExecIntermediateNode
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
}