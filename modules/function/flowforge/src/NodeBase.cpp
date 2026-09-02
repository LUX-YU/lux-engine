/**
 * @file Node.cpp
 * @brief Implements classes and methods defined in Node.hpp for the FlowForge node and pin system.
 */
#include "lux/engine/meta/RuntimeObject.hpp"
#include <cstdio>
#include <lux/engine/flowforge/graph/NodeBase.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
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
        : kind_(kind), name_(name), node_(node)
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

    Pin::~Pin()
    {
        if (node_ == nullptr)
            return;
        if (kind_ == EPinKind::DATA_IN || kind_ == EPinKind::EXEC_IN)
            node_->removeInPin(this);
        else
            node_->removeOutPin(this);
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
        if (pin == nullptr || node_ == nullptr || pin->node_ == nullptr || node_->graph() == nullptr ||
            node_->graph() != pin->node_->graph())
        {
            return ELinkError::INVALID_PIN;
        }
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
    ELinkError Pin::linkTo(Pin* pin, LastLink& last)
    {
        last = {};
        return node_->graph()->connect(*this, *pin);
    }

    /**
     * @brief Unlinks this Pin from the specified Pin.
     *        Base class does nothing and returns SUCCESS.
     * @param pin Pointer to the Pin to unlink from.
     * @return Always ELinkError::SUCCESS in the base class.
     */
    ELinkError Pin::unlinkFrom(Pin* pin)
    {
        if (pin == nullptr || node_ == nullptr || node_->graph() == nullptr)
            return ELinkError::INVALID_PIN;
        return node_->graph()->disconnect(*this, *pin);
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
    PinId Pin::id() const
    {
        return id_;
    }

    /**
     * @brief Assigns an ID to this Pin.
     * @param id The ID value to set.
     */
    void Pin::setId(PinId id)
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
    // Structural links live exclusively in FlowGraph::topology().
    ExecInPin::~ExecInPin() = default;

    /**
     * @brief Checks if a specific ExecOutPin is linked to this ExecInPin.
     * @param out_pin Pointer to the ExecOutPin to check.
     * @return True if the ExecOutPin is linked, otherwise false.
     */
    bool ExecInPin::hasPin(const ExecOutPin* out_pin) const
    {
        const auto pins = linkedPins();
        return std::ranges::find(pins, out_pin) != pins.end();
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

        return Pin::linkTo(pin, last);
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
        return Pin::unlinkFrom(pin);
    }

    /**
     * @brief Retrieves the list of ExecOutPins linked to this ExecInPin.
     * @return A constant reference to a vector of ExecOutPin pointers.
     */
    std::vector<ExecOutPin*> ExecInPin::linkedPins() const
    {
        std::vector<ExecOutPin*> result;
        if (node()->graph() == nullptr)
            return result;
        for (auto* pin : node()->graph()->linkedPins(id()))
            if (pin != nullptr && pin->kind() == EPinKind::EXEC_OUT)
                result.push_back(static_cast<ExecOutPin*>(pin));
        return result;
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
    ExecOutPin::~ExecOutPin() = default;

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

        if (nextPin() != nullptr)
            return ELinkError::HAS_LINKED;

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
        auto can_link_result = canLink(pin);
        if (can_link_result != ELinkError::SUCCESS)
        {
            return can_link_result;
        }

        return Pin::linkTo(pin, last);
    }

    /**
     * @brief Unlinks this ExecOutPin from the specified Pin (usually an ExecInPin).
     * @param pin Pointer to the Pin to unlink from.
     * @return ELinkError describing the result of the unlink operation.
     */
    ELinkError ExecOutPin::unlinkFrom(Pin* pin)
    {
        if (pin == nullptr || pin->kind() != EPinKind::EXEC_IN)
            return ELinkError::WRONG_KIND;
        return Pin::unlinkFrom(pin);
    }

    /**
     * @brief Retrieves the ExecInPin currently linked to this ExecOutPin.
     * @return A pointer to the ExecInPin, or nullptr if none is linked.
     */
    const ExecInPin* ExecOutPin::nextPin() const
    {
        if (node()->graph() == nullptr)
            return nullptr;
        const auto pins = node()->graph()->linkedPins(id());
        return pins.empty() ? nullptr : static_cast<const ExecInPin*>(pins.front());
    }

    ExecInPin* ExecOutPin::nextPin()
    {
        return const_cast<ExecInPin*>(std::as_const(*this).nextPin());
    }

    // ====================== DataInPin ======================

    /**
     * @brief Constructs a DataInPin for the specified Node and type info.
     *        Optionally sets a name based on type info if applicable.
     * @param node Pointer to the parent Node.
     * @param type A type_info_ptr_t describing the pin's data type.
     */
    // The default constant is a ZERO value of the pin's type (invalid for
    // types that need a constructor). NOTE: RuntimeObject(info.type) would
    // be wrong here — the pointer matches RuntimeObject's SBO template and
    // gets stored as the VALUE (the old form of this bug produced garbage
    // "constants" that were really the RefType pointer's low bits).
    DataInPin::DataInPin(Node* node, const DataPinInfo& info, bool allow_default, bool is_necessary)
        : Pin(node, EPinKind::DATA_IN, info.name),
          info_(info),
          allow_default_(allow_default),
		  is_necessary_(is_necessary),
          data_(lux::meta::RuntimeObject::defaultOf(info.type)){}

    /**
     * @brief Destructor. Unlinks from the connected DataOutPin upon destruction.
     */
    DataInPin::~DataInPin() = default;

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

        if (linkedPin() != nullptr)
            return ELinkError::HAS_LINKED;

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

        return Pin::linkTo(pin, last);
    }

    /**
     * @brief Unlinks this DataInPin from the specified Pin (usually a DataOutPin).
     * @param pin Pointer to the Pin to unlink from.
     * @return ELinkError describing the result of the unlink operation.
     */
    ELinkError DataInPin::unlinkFrom(Pin* pin)
    {
        if (pin == nullptr || pin->kind() != EPinKind::DATA_OUT)
            return ELinkError::WRONG_KIND;
        return Pin::unlinkFrom(pin);
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
        // Zero-initialized default of the pin's type (see the ctor note —
        // RuntimeObject(info_.type) would store the pointer as the value).
        data_ = lux::meta::RuntimeObject::defaultOf(info_.type);
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
        if (node()->graph() == nullptr)
            return nullptr;
        const auto pins = node()->graph()->linkedPins(id());
        return pins.empty() ? nullptr : static_cast<const DataOutPin*>(pins.front());
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
    // Structural links live exclusively in FlowGraph::topology().
    DataOutPin::~DataOutPin() = default;

    /**
     * @brief Checks if this DataOutPin already has a specified DataInPin linked.
     * @param in_pin Pointer to the DataInPin.
     * @return True if the DataInPin is linked, otherwise false.
     */
    bool DataOutPin::hasPin(const DataInPin* in_pin) const
    {
        const auto pins = linkPins();
        return std::ranges::find(pins, in_pin) != pins.end();
    }

    /**
     * @brief Retrieves all DataInPins linked to this DataOutPin.
     * @return A constant reference to the vector of DataInPin pointers.
     */
    std::vector<DataInPin*> DataOutPin::linkPins() const
    {
        std::vector<DataInPin*> result;
        if (node()->graph() == nullptr)
            return result;
        for (auto* pin : node()->graph()->linkedPins(id()))
            if (pin != nullptr && pin->kind() == EPinKind::DATA_IN)
                result.push_back(static_cast<DataInPin*>(pin));
        return result;
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

        return Pin::linkTo(pin, last);
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

        return Pin::unlinkFrom(pin);
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
        : operation_(ENodeOperation::INVALID)
    {
    }

    /**
     * @brief Constructs a Node with a given ID and operation type.
     * @param id The unique ID for the Node.
     * @param op The operation type, e.g., START, BRANCH, etc.
     */
    Node::Node(uint64_t id, ENodeOperation op)
        : id_(NodeId{id})
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
    NodeId Node::id() const
    {
        return id_;
    }

    void Node::assignStableId(NodeId id)
    {
        id_ = id;
    }

    void Node::assignGraph(FlowGraph* graph) noexcept
    {
        graph_ = graph;
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
        pin->setId({});
        if (graph_ != nullptr)
            static_cast<void>(graph_->registerPin(*pin));
    }

    /**
     * @brief Adds a Pin to this Node's output pins, and sets the Pin's unique ID.
     * @param pin Pointer to the Pin to add.
     */
    void Node::addOutPin(Pin* pin)
    {
        out_pins_.push_back(pin);
        pin->setId({});
        if (graph_ != nullptr)
            static_cast<void>(graph_->registerPin(*pin));
    }

    /**
     * @brief Removes a Pin from this Node's input pins.
     * @param pin Pointer to the Pin to remove.
     */
    void Node::removeInPin(Pin* pin)
    {
        if (graph_ != nullptr && pin != nullptr)
            graph_->unregisterPin(*pin);
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
        if (graph_ != nullptr && pin != nullptr)
            graph_->unregisterPin(*pin);
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
