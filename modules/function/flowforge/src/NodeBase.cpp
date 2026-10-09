/**
 * @file Node.cpp
 * @brief Implements classes and methods defined in Node.hpp for the FlowForge node and pin system.
 */
#include "lux/engine/meta/RuntimeObject.hpp"
#include <cstdio>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/NodeBase.hpp>
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
    Pin::Pin(Node* node, EPinKind kind, std::string_view name) : kind_(kind), name_(name), node_(node)
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
        {
            return;
        }
        if (kind_ == EPinKind::DATA_IN || kind_ == EPinKind::EXEC_IN)
        {
            node_->removeInPin(this);
        }
        else
        {
            node_->removeOutPin(this);
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
     * @brief Sets the name of this Pin.
     * @param name A string_view representing the new name.
     */
    void Pin::setName(std::string_view name)
    {
        name_ = name;
    }

    // ====================== ExecInPin ======================

    /**
     * @brief Constructs an ExecInPin for the specified Node.
     * @param node Pointer to the parent Node.
     */
    ExecInPin::ExecInPin(Node* node, std::string_view name) : Pin(node, EPinKind::EXEC_IN, name) {}

    /**
     * @brief Destructor. Unlinks from all connected ExecOutPins upon destruction.
     */
    // Structural links live exclusively in FlowGraph::topology().
    ExecInPin::~ExecInPin() = default;

    /**
     * @brief Retrieves the list of ExecOutPins linked to this ExecInPin.
     * @return A constant reference to a vector of ExecOutPin pointers.
     */
    std::vector<ExecOutPin*> ExecInPin::linkedPins() const
    {
        std::vector<ExecOutPin*> result;
        if (node()->graph() == nullptr)
        {
            return result;
        }
        for (auto* pin : node()->graph()->linkedPins(node()->graph()->pinId(this)))
        {
            if (pin != nullptr && pin->kind() == EPinKind::EXEC_OUT)
            {
                result.push_back(static_cast<ExecOutPin*>(pin));
            }
        }
        return result;
    }

    // ====================== ExecOutPin ======================

    /**
     * @brief Constructs an ExecOutPin for the specified Node.
     * @param node Pointer to the parent Node.
     */
    ExecOutPin::ExecOutPin(Node* node, std::string_view name) : Pin(node, EPinKind::EXEC_OUT, name) {}

    /**
     * @brief Destructor. Unlinks from the connected ExecInPin upon destruction.
     */
    ExecOutPin::~ExecOutPin() = default;

    /**
     * @brief Retrieves the ExecInPin currently linked to this ExecOutPin.
     * @return A pointer to the ExecInPin, or nullptr if none is linked.
     */
    const ExecInPin* ExecOutPin::nextPin() const
    {
        if (node()->graph() == nullptr)
        {
            return nullptr;
        }
        const auto pins = node()->graph()->linkedPins(node()->graph()->pinId(this));
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
        : Pin(node, EPinKind::DATA_IN, info.name), info_(info), allow_default_(allow_default),
          is_necessary_(is_necessary)
    {
        // A pin may require a link rather than a literal. Automatic defaults are optional;
        // signature/type validation remains the graph's responsibility. Explicit reset reports failure.
        (void)resetConstantData();
    }

    /**
     * @brief Destructor. Unlinks from the connected DataOutPin upon destruction.
     */
    DataInPin::~DataInPin() = default;

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

    lux::cxx::expected<void, lux::meta::ERuntimeObjectError> DataInPin::resetConstantData() noexcept
    {
        if (!info_.type)
        {
            return lux::cxx::unexpected(lux::meta::ERuntimeObjectError::INVALID_TYPE);
        }
        auto candidate = lux::meta::RuntimeObject::defaultOf(*info_.type);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        data_ = std::move(*candidate);
        return {};
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
        {
            return nullptr;
        }
        const auto pins = node()->graph()->linkedPins(node()->graph()->pinId(this));
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
        : Pin(node, EPinKind::DATA_OUT, name.empty() ? info.name : std::move(name)), info_(info)
    {
    }

    /**
     * @brief Destructor. Unlinks from all connected DataInPins upon destruction.
     */
    // Structural links live exclusively in FlowGraph::topology().
    DataOutPin::~DataOutPin() = default;

    /**
     * @brief Retrieves all DataInPins linked to this DataOutPin.
     * @return A constant reference to the vector of DataInPin pointers.
     */
    std::vector<DataInPin*> DataOutPin::linkPins() const
    {
        std::vector<DataInPin*> result;
        if (node()->graph() == nullptr)
        {
            return result;
        }
        for (auto* pin : node()->graph()->linkedPins(node()->graph()->pinId(this)))
        {
            if (pin != nullptr && pin->kind() == EPinKind::DATA_IN)
            {
                result.push_back(static_cast<DataInPin*>(pin));
            }
        }
        return result;
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
     * @brief Default constructor for an invalid Node (operation = INVALID).
     */
    Node::Node() : operation_(ENodeOperation::INVALID) {}

    Node::Node(ENodeOperation op) : operation_(op) {}

    /**
     * @brief Virtual destructor for Node. Pins are automatically unlinked via their destructors.
     */
    Node::~Node() = default;

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
        if (graph_ != nullptr)
        {
            static_cast<void>(graph_->registerPin(*pin));
        }
    }

    /**
     * @brief Adds a Pin to this Node's output pins, and sets the Pin's unique ID.
     * @param pin Pointer to the Pin to add.
     */
    void Node::addOutPin(Pin* pin)
    {
        out_pins_.push_back(pin);
        if (graph_ != nullptr)
        {
            static_cast<void>(graph_->registerPin(*pin));
        }
    }

    /**
     * @brief Removes a Pin from this Node's input pins.
     * @param pin Pointer to the Pin to remove.
     */
    void Node::removeInPin(Pin* pin)
    {
        if (graph_ != nullptr && pin != nullptr)
        {
            graph_->unregisterPin(*pin);
        }
        in_pins_.erase(std::remove(in_pins_.begin(), in_pins_.end(), pin), in_pins_.end());
    }

    /**
     * @brief Removes a Pin from this Node's output pins.
     * @param pin Pointer to the Pin to remove.
     */
    void Node::removeOutPin(Pin* pin)
    {
        if (graph_ != nullptr && pin != nullptr)
        {
            graph_->unregisterPin(*pin);
        }
        out_pins_.erase(std::remove(out_pins_.begin(), out_pins_.end(), pin), out_pins_.end());
    }

    /**
     * @brief Sets the user-defined name of this Node.
     * @param name A string_view representing the new name.
     */
    void Node::setName(std::string_view name)
    {
        name_ = name;
    }

    ExecIntermediateNode::ExecIntermediateNode(
        ENodeOperation op,
        std::string_view in_pin_name,
        std::string_view fix_out_pin_name,
        std::initializer_list<std::string_view> out_pin_names
    )
        : Node(op), THasExecInPin(in_pin_name), THasExecOutPin(fix_out_pin_name, out_pin_names)
    {
    }

} // namespace lux::flowforge
