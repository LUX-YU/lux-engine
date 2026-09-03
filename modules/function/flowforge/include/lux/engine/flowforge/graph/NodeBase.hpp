/**
 * @file Node.hpp
 * @brief Defines classes and enumerations for FlowForge node and pin operations in the Lux engine.
 */

#pragma once

#include <limits>
#include <cstdint>
#include <span>
#include <string>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/function/graph/GraphTopology.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>


 /**
  * @namespace lux::flowforge
  * @brief Contains classes and functions that represent a flow graph system with nodes and pins.
  */
namespace lux::flowforge
{
    class FlowGraph;

    using lux::graph::NodeId;
    using lux::graph::PinId;

    /**
     * @enum ENodeOperation
     * @brief Enumerates common or critical node operation types in the flow graph.
     *
     * For any new built-in keyword, add it here as needed.
     */
    enum class ENodeOperation : uint8_t
    {
        INVALID = 0,      ///< Undefined or placeholder operation.
        START,            // 0 exec in, 1 exec out
        BRANCH,           // 1 exec in, 2 exec out (true/false). 1 data in (bool).
        SEQUENCE,         // 1 exec in, n exec out.
        FOR_LOOP,         // 1 exec in, 2 exec out (loop body), exec out (completed). 2 data in (first/last index). 1 data out (index).
        WHILE_LOOP,       // 1 exec in, 2 exec out (loop body), exec out (completed). 1 data in (bool condition). 1 data out (index).
		RETURN,           // 1 exec in, 0 exec out. n data in (return value).
        BREAK,            // 1 exec in, 0 exec out. Exits the innermost enclosing loop.

        // functional
        FUNC_DEF_START,   // 0 exec in, 1 exec out (return). n data out (arguments).
        FUNC_RETURN,      // 1 exec in, 0 exec out. n data in (return value).
        NATIVE_FUNC_CALL, // 1 exec in, 1 exec out. n data in (arguments). n data out (return value).
        GRAPH_FUNC_CALL,  // 1 exec in, 1 exec out. n data in (arguments). n data out (return values). Calls a FuncDef in the same graph.
        SCRIPT_ABILITY_CALL, // Explicit Script Ability call; identity is ContractId + MethodId.
        SCRIPT_EVENT_WAIT, // Explicit one-shot Script Event wait; concrete identity is owned by its domain projection.

        // object — the graph never OWNS objects: object references are
        // engine-provided pointers (event/function parameters). CREATE_OBJECT
        // is reserved but deliberately unimplemented (allocation/destruction
        // semantics undecided; use a reflected factory native call instead).
        CREATE_OBJECT,    // reserved, unimplemented.
        GET_OBJECT,       // 0 exec in, 0 exec out. 1 data out (object reference).
        SET_OBJECT,       // 1 exec in, 1 exec out. 1 data in (set value). 1 data out (object reference).
        GET_FIELD,        // pure (memory read). 1 data in (object reference). 1 data out (field value).
        SET_FIELD,        // 1 exec in, 1 exec out. 1 data in (object reference) + 1 data in (value). 1 data out (object passthrough).

        // Operations/Mathematical — PURE nodes: no exec pins, re-evaluated on
        // demand at every use site (UE-style dataflow semantics).
        ADD,              // pure. 2 data in (operands). 1 data out (result).
        SUBTRACT,         // pure. 2 data in (operands). 1 data out (result).
        MULTIPLY,         // pure. 2 data in (operands). 1 data out (result).
        DIVIDE,           // pure. 2 data in (operands). 1 data out (result).
        MODULO,           // pure. 2 data in (operands). 1 data out (result).
        LOGICAL_AND,      // pure. 2 data in (operands, bool). 1 data out (bool).
        LOGICAL_OR,       // pure. 2 data in (operands, bool). 1 data out (bool).
        LOGICAL_NOT,      // pure. 1 data in (operand, bool).  1 data out (bool).
        NEGATE,           // pure. 1 data in (operand).        1 data out (result).

        // Comparisons — pure, result is always bool.
        CMP_EQ,           // pure. 2 data in (operands). 1 data out (bool).
        CMP_NE,           // pure. 2 data in (operands). 1 data out (bool).
        CMP_LT,           // pure. 2 data in (operands). 1 data out (bool).
        CMP_LE,           // pure. 2 data in (operands). 1 data out (bool).
        CMP_GT,           // pure. 2 data in (operands). 1 data out (bool).
        CMP_GE,           // pure. 2 data in (operands). 1 data out (bool).

        // Graph-local variables. GET is pseudo-pure: it reads the variable
        // slot at every use (never cached), so a Set earlier on the exec
        // chain is always observed.
        GET_VARIABLE,     // pure (memory read). 1 data out (value).
        SET_VARIABLE,     // 1 exec in, 1 exec out. 1 data in (value). 1 data out (value passthrough).

        // EVENT
        ON_EVENT,         // 0 exec in, 1 exec out, 1 data out (event payload).
        SEND_EVENT        // 1 exec in, 0 exec out. 1 data in (event payload).
    };

    /**
     * @brief Converts an ENodeOperation to a string representation.
     *
     * @param op The ENodeOperation value.
     * @return A C-string describing the operation type.
     */
    static inline const char* toString(ENodeOperation op)
    {
        switch (op)
        {
        case ENodeOperation::INVALID:       return "Invalid";
        case ENodeOperation::START:         return "Start";
        case ENodeOperation::BRANCH:        return "Branch";
        case ENodeOperation::SEQUENCE:      return "Sequence";
        case ENodeOperation::FOR_LOOP:      return "For Loop";
        case ENodeOperation::WHILE_LOOP:    return "While Loop";
        case ENodeOperation::BREAK:         return "Break";
        case ENodeOperation::FUNC_DEF_START:return "Function Definition Start";
        case ENodeOperation::FUNC_RETURN:   return "Function Return";
        case ENodeOperation::NATIVE_FUNC_CALL:return "Native Function Call";
        case ENodeOperation::GRAPH_FUNC_CALL: return "Graph Function Call";
        case ENodeOperation::SCRIPT_ABILITY_CALL: return "Script Ability Call";
        case ENodeOperation::SCRIPT_EVENT_WAIT: return "Script Event Wait";
        case ENodeOperation::CREATE_OBJECT: return "Create Object";
        case ENodeOperation::GET_OBJECT:    return "Get Object";
        case ENodeOperation::SET_OBJECT:    return "Set Object";
        case ENodeOperation::GET_FIELD:     return "Get Field";
        case ENodeOperation::SET_FIELD:     return "Set Field";
        case ENodeOperation::ADD:           return "Add";
        case ENodeOperation::SUBTRACT:      return "Subtract";
        case ENodeOperation::MULTIPLY:      return "Multiply";
        case ENodeOperation::DIVIDE:        return "Divide";
        case ENodeOperation::MODULO:        return "Modulo";
        case ENodeOperation::LOGICAL_AND:   return "Logical And";
        case ENodeOperation::LOGICAL_OR:    return "Logical Or";
        case ENodeOperation::LOGICAL_NOT:   return "Logical Not";
        case ENodeOperation::NEGATE:        return "Negate";
        case ENodeOperation::CMP_EQ:        return "Equal";
        case ENodeOperation::CMP_NE:        return "Not Equal";
        case ENodeOperation::CMP_LT:        return "Less";
        case ENodeOperation::CMP_LE:        return "Less Equal";
        case ENodeOperation::CMP_GT:        return "Greater";
        case ENodeOperation::CMP_GE:        return "Greater Equal";
        case ENodeOperation::GET_VARIABLE:  return "Get Variable";
        case ENodeOperation::SET_VARIABLE:  return "Set Variable";
        case ENodeOperation::RETURN:        return "Return";
        case ENodeOperation::ON_EVENT:      return "On Event";
        case ENodeOperation::SEND_EVENT:    return "Send Event";
        default:                            return "Unknown";
        }
    }

    /**
     * @brief True for PURE data nodes — no exec pins; their outputs are
     *        re-evaluated on demand wherever they are used (the MLIR
     *        lowering expands the pure subgraph per region).
     */
    static inline bool isPureDataOp(ENodeOperation op)
    {
        switch (op)
        {
        case ENodeOperation::ADD:
        case ENodeOperation::SUBTRACT:
        case ENodeOperation::MULTIPLY:
        case ENodeOperation::DIVIDE:
        case ENodeOperation::MODULO:
        case ENodeOperation::LOGICAL_AND:
        case ENodeOperation::LOGICAL_OR:
        case ENodeOperation::LOGICAL_NOT:
        case ENodeOperation::NEGATE:
        case ENodeOperation::CMP_EQ:
        case ENodeOperation::CMP_NE:
        case ENodeOperation::CMP_LT:
        case ENodeOperation::CMP_LE:
        case ENodeOperation::CMP_GT:
        case ENodeOperation::CMP_GE:
        case ENodeOperation::GET_VARIABLE:
        case ENodeOperation::GET_FIELD:   // pseudo-pure memory read (never cached)
        case ENodeOperation::GET_OBJECT:
            return true;
        default:
            return false;
        }
    }

    /**
     * @enum EPinKind
     * @brief Determines the kind of pin (Exec or Data).
     */
    enum class EPinKind : uint8_t
    {
        UNKNOWN,   ///< Pin kind not specified.
        EXEC_IN,   ///< Executable input pin.
        EXEC_OUT,  ///< Executable output pin.
        DATA_IN,   ///< Data input pin.
        DATA_OUT   ///< Data output pin.
    };

    /**
     * @enum ELinkError
     * @brief Describes the result of a link operation between pins.
     */
    enum class ELinkError
    {
        SUCCESS,       ///< Link operation succeeded.
        INVALID_PIN,   ///< The pin was invalid.
        WRONG_KIND,    ///< Linking pins of incompatible kinds.
        HAS_LINKED,    ///< Pins are already linked.
        SAME_NODE,     ///< Attempting to link pins on the same node where it isn't allowed.
        UNLINKED,      ///< The pins were unlinked successfully.
		UNMATCHED,     ///< The pins are not linked.
        UNKNOWN        ///< An unknown error occurred.
    };

    /**
     * @struct LastLink
     * @brief Stores information about the most recent link changes (for undo or tracking).
     */
    struct LastLink
    {
        bool      exist{false};    ///< Indicates if a previous link existed.
        PinId in_pin_id{};  ///< ID of the input pin previously linked.
        PinId out_pin_id{}; ///< ID of the output pin previously linked.
    };

    // Forward declarations
    class Node;
    class ExecInPin;
    class ExecOutPin;
    class DataInPin;
    class DataOutPin;

    /**
     * @struct DataPinInfo
     * @brief Contains type information for data pins.
     */
    struct DataPinInfo
    {
        std::string                name;
        const lux::meta::RefType*  type;
    };

    /**
     * @class Pin
     * @brief The base class for all pin types in the flow graph.
     *
     * A Pin holds a reference to its parent Node and maintains
     * a kind (Exec/Data) and a name. Derived classes handle
     * specific logic for linking and unlinking pins.
     */
    class Pin
    {
        friend class Node;
        friend class FlowGraph;

    public:
        /**
         * @brief Constructs a Pin object with the specified node, kind, and optional name.
         * @param node Pointer to the parent Node.
         * @param kind The kind of this Pin (Exec/Data, input/output).
         * @param name A string label for the Pin.
         */
        Pin(Node* node, EPinKind kind, std::string_view name = "");

        /**
         * @brief Virtual destructor for Pin.
         */
        virtual ~Pin();

        /**
         * @brief Gets the kind of this Pin.
         * @return The EPinKind value.
         */
        EPinKind kind() const;

        /**
         * @brief Gets the name of this Pin.
         * @return A const reference to the name string.
         */
        const std::string& name() const;

        /**
         * @brief Checks if this Pin can be linked to another Pin.
         * @param pin The other Pin to be checked.
         * @return An ELinkError indicating the result of the check.
         */
        virtual ELinkError canLink(Pin* pin) const;

        /**
         * @brief Links this Pin to another Pin.
         * @param pin The other Pin to link with.
         * @param last Holds information about any previous link that was replaced.
         * @return An ELinkError indicating success or the type of error.
         */
        virtual ELinkError linkTo(Pin* pin, LastLink& last);

        /**
         * @brief Unlinks this Pin from another Pin.
         * @param pin The other Pin to unlink from.
         * @return An ELinkError indicating success or the type of error.
         */
        virtual ELinkError unlinkFrom(Pin* pin);

        /**
         * @brief Gets the parent Node of this Pin.
         * @return A pointer to the Node.
         */
        Node* node() const;

        /**
         * @brief Sets the name of this Pin.
         * @param name The new name string.
         */
        void setName(std::string_view name);

        /**
         * @brief Gets the unique ID of this Pin.
         * @return A 64-bit integer representing the ID.
         */
        PinId id() const;

    protected:
        /**
         * @brief Assigns a unique ID to this Pin.
         * @param id The new ID value.
         */
        void setId(PinId id);

    private:
        PinId       id_; ///< Stable shared-topology identity.
        EPinKind    kind_;             ///< The kind of this pin.
        std::string name_;             ///< A user-defined name for this pin.
        Node*       node_;             ///< Pointer to the parent Node.
    };

    /**
     * @class ExecInPin
     * @brief Represents an executable input pin.
     *
     * This pin can be linked to ExecOutPins. It can store references
     * to multiple ExecOutPins (fan-in).
     */
    class ExecInPin : public Pin
    {
        friend class ExecOutPin;

    public:
        /**
         * @brief Constructs an ExecInPin object for the given node.
         * @param node The parent Node.
         */
        explicit ExecInPin(Node* node, std::string_view name = "->");

        /**
         * @brief Destructor. Unlinks from all connected ExecOutPins on destruction.
         */
        ~ExecInPin() override;

        /**
         * @brief Gets all ExecOutPins linked to this ExecInPin.
         * @return A const reference to a vector of ExecOutPin pointers.
         */
        [[nodiscard]] std::vector<ExecOutPin*> linkedPins() const;

        /**
         * @brief Checks if a given ExecOutPin is already linked.
         * @param node Pointer to the ExecOutPin to check.
         * @return True if linked, otherwise false.
         */
        bool hasPin(const ExecOutPin* node) const;

        /**
         * @brief Checks if this ExecInPin can link to another Pin.
         * @param pin The other Pin to check.
         * @return An ELinkError indicating if linking is possible.
         */
        ELinkError canLink(Pin* pin) const override;

        /**
         * @brief Links this ExecInPin to another Pin (usually an ExecOutPin).
         * @param pin The other Pin to link to.
         * @param last Holds previous link information if any existed.
         * @return An ELinkError indicating the link result.
         */
        ELinkError linkTo(Pin* pin, LastLink& last) override;

        /**
         * @brief Unlinks this ExecInPin from the specified Pin (usually an ExecOutPin).
         * @param pin The Pin to unlink from.
         * @return An ELinkError indicating the unlink result.
         */
        ELinkError unlinkFrom(Pin* pin) override;

    };

    /**
     * @class ExecOutPin
     * @brief Represents an executable output pin.
     *
     * This pin can link to exactly one ExecInPin (fan-out).
     */
    class ExecOutPin : public Pin
    {
        friend class ExecInPin;

    public:
        /**
         * @brief Constructs an ExecOutPin object for the given node.
         * @param node The parent Node.
         */
        explicit ExecOutPin(Node* node, std::string_view name = "->");

        /**
         * @brief Destructor. Unlinks from the connected ExecInPin on destruction.
         */
        ~ExecOutPin() override;

        /**
         * @brief Checks if this ExecOutPin can link to another Pin.
         * @param pin The other Pin to check.
         * @return An ELinkError indicating if linking is possible.
         */
        ELinkError canLink(Pin* pin) const override;

        /**
         * @brief Links this ExecOutPin to another Pin (usually an ExecInPin).
         * @param pin The other Pin to link to.
         * @param last Holds previous link information if any existed.
         * @return An ELinkError indicating the link result.
         */
        ELinkError linkTo(Pin* pin, LastLink& last) override;

        /**
         * @brief Unlinks this ExecOutPin from the specified Pin (usually an ExecInPin).
         * @param pin The Pin to unlink from.
         * @return An ELinkError indicating the unlink result.
         */
        ELinkError unlinkFrom(Pin* pin) override;

        /**
         * @brief Retrieves the ExecInPin linked to this ExecOutPin.
         * @return A pointer to the ExecInPin, or nullptr if none is linked.
         */
        const ExecInPin* nextPin() const;

        ExecInPin* nextPin();

    };

    /**
     * @class DataInPin
     * @brief Represents a data input pin.
     *
     * This pin can link to a DataOutPin, receiving data from it.
     */
    class DataInPin : public Pin
    {
        friend class DataOutPin;

    public:
        /**
         * @brief Constructs a DataInPin object for the given node and type info.
         * @param node The parent Node.
         * @param info The DataPinInfo containing name and type information.
         */
        explicit DataInPin(Node* node, const DataPinInfo& info, bool allow_default = false, bool is_necessary = false);

        /**
         * @brief Destructor. Unlinks from the connected DataOutPin on destruction.
         */
        ~DataInPin() override;

        /**
         * @brief Gets the info of this data pin.
         * @return A const reference to the DataPinInfo.
         */
        const DataPinInfo& info() const;

        /**
         * @brief Gets the DataOutPin linked to this DataInPin.
         * @return A pointer to the connected DataOutPin, or nullptr if none is linked.
         */
        const DataOutPin* linkedPin() const;

        /**
         * @brief Checks if this DataInPin can link to another Pin.
         * @param pin The other Pin to check.
         * @return An ELinkError indicating if linking is possible.
         */
        ELinkError canLink(Pin* pin) const override;

        /**
         * @brief Links this DataInPin to another Pin (usually a DataOutPin).
         * @param pin The other Pin to link with.
         * @param last Holds previous link information if any existed.
         * @return An ELinkError indicating the link result.
         */
        ELinkError linkTo(Pin* pin, LastLink& last) override;

        /**
         * @brief Unlinks this DataInPin from the specified Pin (usually a DataOutPin).
         * @param pin The Pin to unlink from.
         * @return An ELinkError indicating the unlink result.
         */
        ELinkError unlinkFrom(Pin* pin) override;

		/**
		 * @brief Sets the constant data for this DataInPin.
		 * @param value The Constant value to set.
		 * @return True if the data was set successfully, false otherwise.
		 */
        bool setConstantData(lux::meta::RuntimeObject value);
		
        /**
         * @brief Gets the constant data stored in this DataInPin.
         * @return A const reference to the Constant data.
         */
        const lux::meta::RuntimeObject& constantData() const;
        lux::meta::RuntimeObject& constantData();
        
        /**
         * @brief Resets the constant data to a default value based on the pin's type.
         * This creates a new default constant of the appropriate type.
         */
        void resetConstantData();

		/**
		 * @brief Checks if this DataInPin is a constant.
		 * @return True if it is a constant, false otherwise.
		 */
        bool validConstant() const;

		/**
		 * @brief Checks if this DataInPin allows a default value.
		 * @return True if default value is allowed, false otherwise.
		 */
        bool allowDefault() const;

		/**
		 * @brief Checks if this DataInPin is necessary for the operation.
		 * @return True if it is necessary, false otherwise.
		 */
        bool isNecessary() const;

    private:
        DataPinInfo                info_; ///< Type info for this data pin.
		bool                       allow_default_; ///< Flag to allow default value.
        bool 					   is_necessary_; ///< Flag to indicate if this pin is necessary.
        lux::meta::RuntimeObject   data_; ///< Constant data storage.
    };

    /**
     * @class DataOutPin
     * @brief Represents a data output pin.
     *
     * This pin can link to multiple DataInPins (fan-out).
     */
    class DataOutPin : public Pin
    {
        friend class DataInPin;

    public:
        /**
         * @brief Constructs a DataOutPin object for the given node and type info.
         * @param node The parent Node.
         * @param type The runtime type info pointer representing the pin's data type.
         */
        explicit DataOutPin(Node* node, const DataPinInfo& info, std::string name = "");

        /**
         * @brief Destructor. Unlinks from all connected DataInPins on destruction.
         */
        ~DataOutPin();

        /**
         * @brief Checks if this DataOutPin already has a specified DataInPin linked.
         * @param node Pointer to the DataInPin to check.
         * @return True if linked, otherwise false.
         */
        bool hasPin(const DataInPin* node) const;

        /**
         * @brief Gets all DataInPins linked to this DataOutPin.
         * @return A const reference to a vector of DataInPin pointers.
         */
        [[nodiscard]] std::vector<DataInPin*> linkPins() const;

        /**
         * @brief Checks if this DataOutPin can link to another Pin.
         * @param pin The other Pin to check.
         * @return An ELinkError indicating if linking is possible.
         */
        ELinkError canLink(Pin* pin) const override;

        /**
         * @brief Links this DataOutPin to another Pin (usually a DataInPin).
         * @param pin The other Pin to link with.
         * @param last Holds previous link information if any existed.
         * @return An ELinkError indicating the link result.
         */
        ELinkError linkTo(Pin* pin, LastLink& last) override;

        /**
         * @brief Unlinks this DataOutPin from the specified Pin (usually a DataInPin).
         * @param pin The Pin to unlink from.
         * @return An ELinkError indicating the unlink result.
         */
        ELinkError unlinkFrom(Pin* pin) override;

        /**
         * @brief Gets the info of this data pin.
         * @return The type_info_ptr_t for this DataOutPin.
         */
        const DataPinInfo& info() const;

    private:
        DataPinInfo info_; ///< Type info for this data pin.
    };

    /**
     * @class Node
     * @brief Represents a node in the flow graph, containing input and output pins.
     *
     * When a Node is destroyed, all its Pins are also destroyed and unlinked.
     */
    class Node
    {
        friend class Pin;
        // HasExecOutPin manages heap-allocated extra exec-out pins and must be able
        // to de-register them from out_pins_ before deleting them.
        template<typename T> friend class HasExecOutPin;

    public:
        /**
         * @brief Default constructor for an invalid Node.
         */
        Node();

        /**
         * @brief Constructs a Node with a given ID and operation type.
         * @param id The unique ID for this Node.
         * @param op The operation type (e.g., START, BRANCH, etc.).
         */
        Node(uint64_t id, ENodeOperation op);

        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
        Node(Node&&) = delete;
        Node& operator=(Node&&) = delete;

        /**
         * @brief Virtual destructor. Pins will be unlinked automatically.
         */
        virtual ~Node();

        /**
         * @brief Gets the unique ID of this Node.
         * @return The Node's ID.
         */
        NodeId id() const;

        /**
         * @brief Re-keys this node with a stable shared-topology id.
         *        Pin identity is allocated independently by GraphTopology.
         */
        void assignStableId(NodeId id);
        void assignGraph(FlowGraph* graph) noexcept;
        [[nodiscard]] FlowGraph* graph() const noexcept { return graph_; }

        /**
         * @brief Name of the NodeRegistry creator that instantiated this
         *        node (empty for programmatically constructed nodes). The
         *        serializer uses it to re-instantiate registry-backed nodes
         *        (native calls in particular) on load.
         */
        const std::string& creatorName() const { return creator_name_; }
        void setCreatorName(std::string_view name) { creator_name_ = name; }

        /**
         * @brief Gets the operation type of this Node.
         * @return An ENodeOperation value.
         */
        ENodeOperation operation() const;

        /**
         * @brief Rebuilds this node's DYNAMIC pins from its current configuration
         *        (e.g. a re-resolved reflected signature). Base implementation is a
         *        no-op; nodes with reflection-driven or variable pin sets override it
         *        (see NativeFuncCall). Links on rebuilt pins are dropped (cleanly
         *        unlinked by the pin destructors) — the caller (the editor's
         *        reconstruct path) re-validates and re-applies surviving links.
         */
        virtual void reconstruct() {}

        /**
         * @brief Gets the user-defined name of this Node.
         * @return A const reference to the name string.
         */
        const std::string& name() const;

        /**
         * @brief Sets the user-defined (display) name for this Node. Public:
         *        the editor's rename and the serializer's display-name
         *        restore both need it.
         */
        void setName(std::string_view name);

        /**
         * @brief Gets the input pins of this Node.
         * @return A const reference to a vector of Pin pointers.
         */
        std::vector<Pin*>& inPins(){ return in_pins_; }
        const std::vector<Pin*>& inPins() const { return in_pins_; }

        /**
         * @brief Gets the output pins of this Node.
         * @return A const reference to a vector of Pin pointers.
         */
		std::vector<Pin*>& outPins() { return out_pins_; }
        const std::vector<Pin*>& outPins() const { return out_pins_; }

    protected:
        /**
         * @brief Adds a pin to the list of input pins.
         * @param pin A pointer to the Pin to add.
         */
        void addInPin(Pin* pin);

        /**
         * @brief Adds a pin to the list of output pins.
         * @param pin A pointer to the Pin to add.
         */
        void addOutPin(Pin* pin);

        /**
         * @brief Removes a pin from the list of input pins.
         * @param pin The Pin to remove.
         */
        void removeInPin(Pin* pin);

        /**
         * @brief Removes a pin from the list of output pins.
         * @param pin The Pin to remove.
         */
        void removeOutPin(Pin* pin);

    private:
        std::vector<Pin*> in_pins_;   ///< A list of pointers to this Node's input pins.
        std::vector<Pin*> out_pins_;  ///< A list of pointers to this Node's output pins.

        NodeId            id_;        ///< Stable shared-topology identity.
        FlowGraph*        graph_{};   ///< Borrowed graph while attached.
        ENodeOperation    operation_; ///< The operation type of the Node.
        std::string       name_;      ///< A user-defined name for the Node.
        std::string       creator_name_; ///< NodeRegistry creator that built this node (may be empty).
    };

    template<typename Dervied>
	class HasExecInPin
	{
    public:
        HasExecInPin(std::string_view name)
			: in_pin_(static_cast<Dervied*>(this), name){ }

        ExecInPin& execInPin() { return in_pin_; }
        const ExecInPin& execInPin() const { return in_pin_; }

    private:
        ExecInPin in_pin_;
	};

	template<typename Dervied>
    class HasExecOutPin
    {
    public:
        void addExecOutPin(std::string_view name)
        {
            auto new_pin = new ExecOutPin(static_cast<Dervied*>(this), name);
            extra_out_pins_.push_back(new_pin);
        }

        bool removeExecOutPin(size_t index)
        {
            if (index >= extra_out_pins_.size())
            {
                return false;
            }
            auto* pin = extra_out_pins_[index];
            extra_out_pins_.erase(extra_out_pins_.begin() + index);
            // De-register from the Node's out_pins_ BEFORE destruction — the Pin
            // destructor only unlinks links, it does not remove the node-side entry.
            static_cast<Dervied*>(this)->removeOutPin(pin);
            delete pin;
            return true;
        }

        bool removeExecOutPin(ExecOutPin* pin)
        {
            auto iter = std::find(extra_out_pins_.begin(), extra_out_pins_.end(), pin);
            if (iter != extra_out_pins_.end())
            {
                extra_out_pins_.erase(iter);
                static_cast<Dervied*>(this)->removeOutPin(pin);
                delete pin;
                return true;
            }
            return false;
        }

    public:
        HasExecOutPin(std::string_view name, std::initializer_list<std::string_view> out_pin_names = {})
            : fix_out_pin_(static_cast<Dervied*>(this), name)
        {
            for (auto name : out_pin_names)
            {
                addExecOutPin(name);
            }
        }

        ~HasExecOutPin()
        {
            // Extra pins are heap-allocated and owned here; delete them (their
            // destructors unlink any exec links). No removeOutPin needed — the
            // whole node is being destroyed alongside its pin lists.
            for (auto* pin : extra_out_pins_)
            {
                delete pin;
            }
        }

		ExecOutPin& execOutPin() { return fix_out_pin_; }
        const ExecOutPin& execOutPin() const { return fix_out_pin_; }

		std::span<ExecOutPin*> extraOutPins() { return extra_out_pins_; }
		std::span<const ExecOutPin* const> extraOutPins() const { return extra_out_pins_; }

    private:
		ExecOutPin               fix_out_pin_;
		std::vector<ExecOutPin*> extra_out_pins_;
    };

	class ExecIntermediateNode :
        public Node, 
        public HasExecInPin<ExecIntermediateNode>, 
        public HasExecOutPin<ExecIntermediateNode>
    {
    public:
        ExecIntermediateNode(uint64_t id, ENodeOperation op, 
            std::string_view in_pin_name = "->", std::string_view fix_out_pin_name = "->", std::initializer_list<std::string_view> out_pin_names = {});
    };
} // namespace lux::flowforge
