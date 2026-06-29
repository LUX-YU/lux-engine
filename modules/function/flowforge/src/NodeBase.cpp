/**
 * @file Node.cpp
 * @brief Implements classes and methods defined in Node.hpp for the FlowForge node and pin system.
 */
#include "lux/engine/meta/RuntimeObject.hpp"
#include <cstdio>
#include <lux/engine/flowforge/NodeBase.hpp>
#include <lux/engine/meta/MetaCompat.hpp>
#include <lux/engine/meta/MetaDef.hpp>

namespace lux::flowforge
{
    // ====================== Pin ======================
    /**
     * @brief Constructs a Pin with a specified Node, Pin kind, and optional name.
     *        Adds the Pin to the Node's input or output pin list accordingly.
     * @param node Pointer to the parent Node.
     * @param kind The EPinKind (input/output, exec/data).
     * @param name The optional name for this Pin.
     */
    Pin::Pin(Node* node, EPinKind kind, std::string_view name)
        : id_(0), kind_(kind), name_(name), node_(node)
    {
        if (EPinKind::DATA_IN == kind || EPinKind::EXEC_IN == kind)
        {
            node->addInPin(this);
        }
        else
        {
            node->addOutPin(this);
        }
    }

    /**
     * @brief Retrieves the kind (EPinKind) of this Pin.
     * @return The Pin kind.
     */
    EPinKind Pin::kind() const
    {
        return kind_;
    }

    /**
     * @brief Retrieves the name of this Pin.
     * @return A constant reference to the name string.
     */
    const std::string& Pin::name() const
    {
        return name_;
    }

    /**
     * @brief Retrieves the Node this Pin belongs to.
     * @return A pointer to the parent Node.
     */
    Node* Pin::node() const
    {
        return node_;
    }

    /**
     * @brief Checks if this Pin can link to the specified Pin.
     *        Base implementation checks if both Pins belong to different Nodes.
     * @param pin Pointer to the other Pin to check.
     * @return ELinkError::SAME_NODE if they share the same Node, else SUCCESS.
     */
    ELinkError Pin::canLink(Pin* pin) const
    {
        if (node_ == pin->node_)
        {
            return ELinkError::SAME_NODE;
        }
        return ELinkError::SUCCESS;
    }

    /**
     * @brief Links this Pin to the specified Pin.
     *        Base class does nothing and returns SUCCESS.
     * @param pin Pointer to the Pin to link to.
     * @param last A reference to a LastLink object (unused here).
     * @return Always ELinkError::SUCCESS in the base class.
     */
    ELinkError Pin::linkTo(Pin* /*pin*/, LastLink& /*last*/)
    {
        // Base class does nothing
        return ELinkError::SUCCESS;
    }

    /**
     * @brief Unlinks this Pin from the specified Pin.
     *        Base class does nothing and returns SUCCESS.
     * @param pin Pointer to the Pin to unlink from.
     * @return Always ELinkError::SUCCESS in the base class.
     */
    ELinkError Pin::unlinkFrom(Pin* /*pin*/)
    {
        // Base class does nothing
        return ELinkError::SUCCESS;
    }

    /**
     * @brief Sets the name of this Pin.
     * @param name A string_view representing the new name.
     */
    void Pin::setName(std::string_view name)
    {
        name_ = name;
    }

    /**
     * @brief Retrieves the unique ID of this Pin.
     * @return A 64-bit integer representing the ID.
     */
    uint64_t Pin::id() const
    {
        return id_;
    }

    /**
     * @brief Assigns an ID to this Pin.
     * @param id The ID value to set.
     */
    void Pin::setId(uint64_t id)
    {
        id_ = id;
    }

    // ====================== ExecInPin ======================

    /**
     * @brief Constructs an ExecInPin for the specified Node.
     * @param node Pointer to the parent Node.
     */
    ExecInPin::ExecInPin(Node* node, std::string_view name)
        : Pin(node, EPinKind::EXEC_IN, name)
    {
    }

    /**
     * @brief Destructor. Unlinks from all connected ExecOutPins upon destruction.
     */
    ExecInPin::~ExecInPin()
    {
        for (auto out_pin : linked_pins_)
        {
            if (out_pin)
            {
                out_pin->unlinkFrom(this);
            }
        }
    }

    /**
     * @brief Checks if a specific ExecOutPin is linked to this ExecInPin.
     * @param out_pin Pointer to the ExecOutPin to check.
     * @return True if the ExecOutPin is linked, otherwise false.
     */
    bool ExecInPin::hasPin(const ExecOutPin* out_pin) const
    {
        auto iter = std::find(linked_pins_.begin(), linked_pins_.end(), out_pin);
        return (iter != linked_pins_.end());
    }

    /**
     * @brief Checks if this ExecInPin can link to the specified Pin.
     *        Ensures the Pin is an ExecOutPin and not already linked.
     * @param pin Pointer to the Pin to check.
     * @return An ELinkError code describing the result.
     */
    ELinkError ExecInPin::canLink(Pin* pin) const
    {
        auto rst = Pin::canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        if (pin->kind() != EPinKind::EXEC_OUT)
        {
            return ELinkError::WRONG_KIND;
        }

        auto out_pin = static_cast<ExecOutPin*>(pin);
        if (hasPin(out_pin))
        {
            return ELinkError::HAS_LINKED;
        }

        // If out_pin->nextPin() == this, it means they're already linked
        if (out_pin->nextPin() == this)
        {
            return ELinkError::HAS_LINKED;
        }

        return ELinkError::SUCCESS;
    }

    /**
     * @brief Links this ExecInPin to another Pin (usually an ExecOutPin).
     *        If the ExecOutPin was already linked to a different ExecInPin, that link is recorded and removed.
     * @param pin Pointer to the Pin to link with.
     * @param last A reference to LastLink for storing replaced link info.
     * @return An ELinkError code describing the result.
     */
    ELinkError ExecInPin::linkTo(Pin* pin, LastLink& last)
    {
        auto rst = canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        auto out_pin = static_cast<ExecOutPin*>(pin);
        // If out_pin had a different InPin, store it in last before unlinking
        if (auto old_in = out_pin->nextPin())
        {
            last.exist = true;
            last.in_pin_id = old_in->id();
            last.out_pin_id = id();

            out_pin->unlinkFrom(old_in);
        }

        // Link (ExecOutPin::linkTo also updates linked_pins_)
        return out_pin->linkTo(this, last);
    }

    /**
     * @brief Unlinks this ExecInPin from the specified Pin (usually an ExecOutPin).
     * @param pin Pointer to the Pin to unlink from.
     * @return An ELinkError code describing the unlink result.
     */
    ELinkError ExecInPin::unlinkFrom(Pin* pin)
    {
        if (pin->kind() != EPinKind::EXEC_OUT)
        {
            return ELinkError::WRONG_KIND;
        }
        auto out_pin = static_cast<ExecOutPin*>(pin);

        // Remove the out_pin from linked_pins_
        auto oldSize = linked_pins_.size();
        linked_pins_.erase(
            std::remove(linked_pins_.begin(), linked_pins_.end(), out_pin),
            linked_pins_.end()
        );

        // If actually removed, also unlink the ExecOutPin
        if (linked_pins_.size() < oldSize)
        {
            out_pin->unlinkFrom(this);
            return ELinkError::SUCCESS;
        }
        return ELinkError::UNMATCHED;
    }

    /**
     * @brief Retrieves the list of ExecOutPins linked to this ExecInPin.
     * @return A constant reference to a vector of ExecOutPin pointers.
     */
    const std::vector<ExecOutPin*>& ExecInPin::linkedPins() const
    {
        return linked_pins_;
    }

    // ====================== ExecOutPin ======================

    /**
     * @brief Constructs an ExecOutPin for the specified Node.
     * @param node Pointer to the parent Node.
     */
    ExecOutPin::ExecOutPin(Node* node, std::string_view name)
        : Pin(node, EPinKind::EXEC_OUT, name)
    {
    }

    /**
     * @brief Destructor. Unlinks from the connected ExecInPin upon destruction.
     */
    ExecOutPin::~ExecOutPin()
    {
        if (next_pin_)
        {
            next_pin_->unlinkFrom(this);
        }
        next_pin_ = nullptr;
    }

    /**
     * @brief Checks if this ExecOutPin can link to the specified Pin.
     *        Ensures the Pin is an ExecInPin and not already linked.
     * @param pin Pointer to the other Pin.
     * @return ELinkError describing the result.
     */
    ELinkError ExecOutPin::canLink(Pin* pin) const
    {
        auto rst = Pin::canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        if (pin->kind() != EPinKind::EXEC_IN)
        {
            return ELinkError::WRONG_KIND;
        }

        if (next_pin_ == pin)
        {
            return ELinkError::HAS_LINKED;
        }

        auto in_pin = static_cast<ExecInPin*>(pin);
        if (in_pin->hasPin(this))
        {
            return ELinkError::HAS_LINKED;
        }

        return ELinkError::SUCCESS;
    }

    /**
     * @brief Links this ExecOutPin to another Pin (usually an ExecInPin).
     *        If already linked, unlinks from the old ExecInPin first.
     * @param pin Pointer to the other Pin.
     * @param last A reference to LastLink for storing replaced link info.
     * @return ELinkError describing the result of the link operation.
     */
    ELinkError ExecOutPin::linkTo(Pin* pin, LastLink& last)
    {
        // Avoid recursion by only setting next_pin_ here.
        auto can_link_result = canLink(pin);
        if (can_link_result != ELinkError::SUCCESS)
        {
            return can_link_result;
        }

        // If already linked to a different ExecInPin, unlink it
        if (next_pin_)
        {
            last.exist = true;
            last.in_pin_id = next_pin_->id();
            last.out_pin_id = id();

            next_pin_->unlinkFrom(this);
        }

        next_pin_ = static_cast<ExecInPin*>(pin);
		next_pin_->linked_pins_.push_back(this);
        return ELinkError::SUCCESS;
    }

    /**
     * @brief Unlinks this ExecOutPin from the specified Pin (usually an ExecInPin).
     * @param pin Pointer to the Pin to unlink from.
     * @return ELinkError describing the result of the unlink operation.
     */
    ELinkError ExecOutPin::unlinkFrom(Pin* pin)
    {
        // Only unlink if pin == next_pin_
        if (next_pin_ != pin)
        {
            return ELinkError::UNLINKED;
        }

        // Clear next_pin_
        next_pin_ = nullptr;

        // Also remove this ExecOutPin from the ExecInPin's linked_pins_ list
        auto in_pin = static_cast<ExecInPin*>(pin);
        auto oldSize = in_pin->linked_pins_.size();
        in_pin->linked_pins_.erase(
            std::remove(in_pin->linked_pins_.begin(), in_pin->linked_pins_.end(), this),
            in_pin->linked_pins_.end()
        );
        if (in_pin->linked_pins_.size() < oldSize)
        {
            return ELinkError::SUCCESS;
        }
        return ELinkError::UNKNOWN;
    }

    /**
     * @brief Retrieves the ExecInPin currently linked to this ExecOutPin.
     * @return A pointer to the ExecInPin, or nullptr if none is linked.
     */
    const ExecInPin* ExecOutPin::nextPin() const
    {
        return next_pin_;
    }

    ExecInPin* ExecOutPin::nextPin()
    {
        return next_pin_;
    }

    // ====================== DataInPin ======================

    /**
     * @brief Constructs a DataInPin for the specified Node and type info.
     *        Optionally sets a name based on type info if applicable.
     * @param node Pointer to the parent Node.
     * @param type A type_info_ptr_t describing the pin's data type.
     */
    DataInPin::DataInPin(Node* node, const DataPinInfo& info, bool allow_default, bool is_necessary)
        : Pin(node, EPinKind::DATA_IN, info.name), 
          info_(info), 
          allow_default_(allow_default),
		  is_necessary_(is_necessary),
          data_(info.type){}

    /**
     * @brief Destructor. Unlinks from the connected DataOutPin upon destruction.
     */
    DataInPin::~DataInPin()
    {
        if (linked_pin_)
        {
            linked_pin_->unlinkFrom(this);
        }
    }

    /**
     * @brief Checks if this DataInPin can link to the specified Pin.
     *        Ensures the Pin is a DataOutPin and not already linked.
     * @param pin Pointer to the other Pin.
     * @return ELinkError describing the result of the link check.
     */
    ELinkError DataInPin::canLink(Pin* pin) const
    {
        auto rst = Pin::canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        if (pin->kind() != EPinKind::DATA_OUT)
        {
            return ELinkError::WRONG_KIND;
        }

        if (linked_pin_ == pin)
        {
            return ELinkError::HAS_LINKED;
        }

        auto out_pin = static_cast<DataOutPin*>(pin);
        if (out_pin->hasPin(this))
        {
            return ELinkError::HAS_LINKED;
        }

        if (!lux::meta::canInitialize(info().type, out_pin->info().type))
        {
			return ELinkError::WRONG_KIND;
        }

        return ELinkError::SUCCESS;
    }

    /**
     * @brief Links this DataInPin to another Pin (usually a DataOutPin).
     *        If already linked, unlinks from the old DataOutPin first.
     * @param pin Pointer to the other Pin.
     * @param last A reference to LastLink for storing replaced link info.
     * @return ELinkError describing the result of the link operation.
     */
    ELinkError DataInPin::linkTo(Pin* pin, LastLink& last)
    {
        auto rst = canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        if (linked_pin_)
        {
            last.exist = true;
            last.in_pin_id = linked_pin_->id();
            last.out_pin_id = id();
            linked_pin_->unlinkFrom(this);
        }
        linked_pin_ = static_cast<DataOutPin*>(pin);
        linked_pin_->linked_pins_.push_back(this);
        return ELinkError::SUCCESS;
    }

    /**
     * @brief Unlinks this DataInPin from the specified Pin (usually a DataOutPin).
     * @param pin Pointer to the Pin to unlink from.
     * @return ELinkError describing the result of the unlink operation.
     */
    ELinkError DataInPin::unlinkFrom(Pin* pin)
    {
        if (linked_pin_ != pin)
        {
            return ELinkError::UNLINKED;
        }

        auto out_pin = static_cast<DataOutPin*>(pin);
        auto oldSize = out_pin->linked_pins_.size();
        out_pin->linked_pins_.erase(
            std::remove(out_pin->linked_pins_.begin(), out_pin->linked_pins_.end(), this),
            out_pin->linked_pins_.end()
        );
        if (out_pin->linked_pins_.size() < oldSize)
        {
            linked_pin_ = nullptr;
            return ELinkError::SUCCESS;
        }
        return ELinkError::UNKNOWN;
    }

    bool DataInPin::setConstantData(lux::meta::RuntimeObject value)
    {
        // Use the new meta system for type compatibility check

        if (lux::meta::canAssign(value.type(), info_.type))
        {
            data_ = std::move(value);
            return true;
        }
        return false;
    }

    const lux::meta::RuntimeObject& DataInPin::constantData() const
    {
        return data_;
    }

    lux::meta::RuntimeObject& DataInPin::constantData()
    {
        return data_;
    }

    void DataInPin::resetConstantData()
    {
        // Create a default constant using the pin's type information
        data_ = lux::meta::RuntimeObject(info_.type);
    }

    bool DataInPin::validConstant() const
    {
        // Check if the variant holds a value other than monostate
        return data_.isValid();
    }

    bool DataInPin::allowDefault() const
    {
		return allow_default_;
    }

    bool DataInPin::isNecessary() const
    {
		return is_necessary_;
    }

    /**
     * @brief Retrieves the runtime type info of this DataInPin.
     * @return A constant reference to the DataPinInfo.
     */
    const DataPinInfo& DataInPin::info() const
    {
        return info_;
    }

    /**
     * @brief Retrieves the DataOutPin currently linked to this DataInPin.
     * @return A pointer to the DataOutPin, or nullptr if none is linked.
     */
    const DataOutPin* DataInPin::linkedPin() const
    {
        return linked_pin_;
    }

    // ====================== DataOutPin ======================

    /**
     * @brief Constructs a DataOutPin for the specified Node and type info.
     * @param node Pointer to the parent Node.
     * @param info The DataPinInfo containing name and type info.
     * @param name Optional override name for this pin.
     */
    DataOutPin::DataOutPin(Node* node, const DataPinInfo& info, std::string name)
        : Pin(node, EPinKind::DATA_OUT, name.empty() ? info.name : std::move(name))
        , info_(info)
    {
    }

    /**
     * @brief Destructor. Unlinks from all connected DataInPins upon destruction.
     */
    DataOutPin::~DataOutPin()
    {
        for (auto in_pin : linked_pins_)
        {
            if (in_pin)
            {
                in_pin->unlinkFrom(this);
            }
        }
    }

    /**
     * @brief Checks if this DataOutPin already has a specified DataInPin linked.
     * @param in_pin Pointer to the DataInPin.
     * @return True if the DataInPin is linked, otherwise false.
     */
    bool DataOutPin::hasPin(const DataInPin* in_pin) const
    {
        auto iter = std::find(linked_pins_.begin(), linked_pins_.end(), in_pin);
        return (iter != linked_pins_.end());
    }

    /**
     * @brief Retrieves all DataInPins linked to this DataOutPin.
     * @return A constant reference to the vector of DataInPin pointers.
     */
    const std::vector<DataInPin*>& DataOutPin::linkPins() const
    {
        return linked_pins_;
    }

    /**
     * @brief Checks if this DataOutPin can link to the specified Pin.
     *        Ensures the Pin is a DataInPin and not already linked.
     * @param pin Pointer to the other Pin.
     * @return ELinkError describing the result of the link check.
     */
    ELinkError DataOutPin::canLink(Pin* pin) const
    {
        auto rst = Pin::canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        if (pin->kind() != EPinKind::DATA_IN)
        {
            return ELinkError::WRONG_KIND;
        }

        auto in_pin = static_cast<DataInPin*>(pin);
        if (hasPin(in_pin) || in_pin->linkedPin() == this)
        {
            return ELinkError::HAS_LINKED;
        }

        // Use the new meta system for type compatibility check
        if (!lux::meta::canInitialize(in_pin->info().type, info_.type))
        {
            return ELinkError::UNMATCHED;
        }

        return ELinkError::SUCCESS;
    }

    /**
     * @brief Links this DataOutPin to another Pin (usually a DataInPin).
     *        If the DataInPin is already linked, unlinks it first.
     * @param pin Pointer to the other Pin.
     * @param last A reference to LastLink for storing replaced link info.
     * @return ELinkError describing the result of the link operation.
     */
    ELinkError DataOutPin::linkTo(Pin* pin, LastLink& last)
    {
        auto rst = canLink(pin);
        if (rst != ELinkError::SUCCESS)
        {
            return rst;
        }

        auto in_pin = static_cast<DataInPin*>(pin);
        if (in_pin->linkedPin())
        {
            last.exist = true;
            last.in_pin_id = in_pin->linkedPin()->id();
            last.out_pin_id = id();

            in_pin->unlinkFrom(in_pin->linked_pin_);
        }
        in_pin->linked_pin_ = this;
        linked_pins_.push_back(in_pin);

        return ELinkError::SUCCESS;
    }

    /**
     * @brief Unlinks this DataOutPin from the specified Pin (usually a DataInPin).
     * @param pin Pointer to the Pin to unlink from.
     * @return ELinkError describing the result of the unlink operation.
     */
    ELinkError DataOutPin::unlinkFrom(Pin* pin)
    {
        if (pin->kind() != EPinKind::DATA_IN)
        {
            return ELinkError::WRONG_KIND;
        }

        auto in_pin = static_cast<DataInPin*>(pin);
        auto oldSize = linked_pins_.size();
        linked_pins_.erase(
            std::remove(linked_pins_.begin(), linked_pins_.end(), in_pin),
            linked_pins_.end()
        );
        if (linked_pins_.size() < oldSize)
        {
            in_pin->linked_pin_ = nullptr;
            return ELinkError::SUCCESS;
        }
        return ELinkError::UNLINKED;
    }

    /**
     * @brief Retrieves the runtime type info of this DataOutPin.
     * @return A constant reference to the DataPinInfo.
     */
    const DataPinInfo& DataOutPin::info() const
    {
        return info_;
    }

    // ====================== Node ======================

    /**
     * @brief Default constructor for an invalid Node (operation = INVALID, id = invalid_id).
     */
    Node::Node()
        : id_(invalid_id)
        , operation_(ENodeOperation::INVALID)
    {
    }

    /**
     * @brief Constructs a Node with a given ID and operation type.
     * @param id The unique ID for the Node.
     * @param op The operation type, e.g., START, BRANCH, etc.
     */
    Node::Node(uint64_t id, ENodeOperation op)
        : id_(id)
        , operation_(op)
    {
    }

    /**
     * @brief Virtual destructor for Node. Pins are automatically unlinked via their destructors.
     */
    Node::~Node() = default;

    /**
     * @brief Retrieves the ID of this Node.
     * @return The Node's 64-bit integer ID.
     */
    uint64_t Node::id() const
    {
        return id_;
    }

    /**
     * @brief Retrieves the operation type of this Node.
     * @return An ENodeOperation enum value.
     */
    ENodeOperation Node::operation() const
    {
        return operation_;
    }

    /**
     * @brief Retrieves the user-defined name of this Node.
     * @return A constant reference to the name string.
     */
    const std::string& Node::name() const
    {
        return name_;
    }

    /**
     * @brief Adds a Pin to this Node's input pins, and sets the Pin's unique ID.
     * @param pin Pointer to the Pin to add.
     */
    void Node::addInPin(Pin* pin)
    {
        in_pins_.push_back(pin);
        pin->setId(reinterpret_cast<uintptr_t>(pin));
    }

    /**
     * @brief Adds a Pin to this Node's output pins, and sets the Pin's unique ID.
     * @param pin Pointer to the Pin to add.
     */
    void Node::addOutPin(Pin* pin)
    {
        out_pins_.push_back(pin);
        pin->setId(reinterpret_cast<uintptr_t>(pin));
    }

    /**
     * @brief Removes a Pin from this Node's input pins.
     * @param pin Pointer to the Pin to remove.
     */
    void Node::removeInPin(Pin* pin)
    {
        in_pins_.erase(
            std::remove(in_pins_.begin(), in_pins_.end(), pin),
            in_pins_.end()
        );
    }

    /**
     * @brief Removes a Pin from this Node's output pins.
     * @param pin Pointer to the Pin to remove.
     */
    void Node::removeOutPin(Pin* pin)
    {
        out_pins_.erase(
            std::remove(out_pins_.begin(), out_pins_.end(), pin),
            out_pins_.end()
        );
    }

    /**
     * @brief Sets the user-defined name of this Node.
     * @param name A string_view representing the new name.
     */
    void Node::setName(std::string_view name)
    {
        name_ = name;
    }

    ExecIntermediateNode::ExecIntermediateNode(uint64_t id, ENodeOperation op, std::string_view in_pin_name, 
        std::string_view fix_out_pin_name, std::initializer_list<std::string_view> out_pin_names)
        : Node(id, op), HasExecInPin(in_pin_name), HasExecOutPin(fix_out_pin_name, out_pin_names) { }

} // namespace lux::flowforge
