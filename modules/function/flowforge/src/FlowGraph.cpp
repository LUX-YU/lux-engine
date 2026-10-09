#include <exception>
#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/detail/FlowNodeIdentity.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/meta/MetaCompat.hpp>
#include <utility>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] lux::graph::NodeTypeId nodeType(const Node& node) noexcept
        {
            if (const auto* type = node.registeredType())
            {
                return type->identity().id;
            }
            return lux::graph::nodeTypeId(detail::builtinNodeName(node.operation()));
        }

        [[nodiscard]] bool isInput(EPinKind kind) noexcept
        {
            return kind == EPinKind::EXEC_IN || kind == EPinKind::DATA_IN;
        }

        [[nodiscard]] std::uint8_t fanCap(EPinKind kind) noexcept
        {
            return kind == EPinKind::EXEC_OUT || kind == EPinKind::DATA_IN ? 1U : lux::graph::kUnlimitedFan;
        }

        [[nodiscard]] lux::graph::PinSemanticId pinSemantic(const Pin& pin) noexcept
        {
            if (pin.node()->registeredType())
            {
                return pin.node()->registeredPinSemantic(pin);
            }
            const auto& pins = isInput(pin.kind()) ? pin.node()->inPins() : pin.node()->outPins();
            const auto found = std::ranges::find(pins, std::addressof(pin));
            return found == pins.end() ? graph::PinSemanticId{}
                                       : detail::builtinPinSemantic(pin.kind(), found - pins.begin());
        }

        // Structural membership, same-node rules and fan limits belong to the graph.
        // Keep the established directional error classification without virtual Pin callbacks.
        [[nodiscard]] ELinkError pinConnection(
            const Pin& first,
            const Pin& second,
            bool has_link,
            bool has_exact_link
        ) noexcept
        {
            switch (first.kind())
            {
            case EPinKind::EXEC_IN:
                if (second.kind() != EPinKind::EXEC_OUT)
                {
                    return ELinkError::WRONG_KIND;
                }
                return has_exact_link ? ELinkError::HAS_LINKED : ELinkError::SUCCESS;
            case EPinKind::EXEC_OUT:
                if (second.kind() != EPinKind::EXEC_IN)
                {
                    return ELinkError::WRONG_KIND;
                }
                return has_link ? ELinkError::HAS_LINKED : ELinkError::SUCCESS;
            case EPinKind::DATA_IN:
            {
                if (second.kind() != EPinKind::DATA_OUT)
                {
                    return ELinkError::WRONG_KIND;
                }
                if (has_link)
                {
                    return ELinkError::HAS_LINKED;
                }
                const auto* input = static_cast<const DataInPin&>(first).info().type;
                const auto* output = static_cast<const DataOutPin&>(second).info().type;
                const bool accepts_type = meta::canInitialize(input, output);
                return accepts_type ? ELinkError::SUCCESS : ELinkError::WRONG_KIND;
            }
            case EPinKind::DATA_OUT:
            {
                if (second.kind() != EPinKind::DATA_IN)
                {
                    return ELinkError::WRONG_KIND;
                }
                if (has_exact_link)
                {
                    return ELinkError::HAS_LINKED;
                }
                const auto* input = static_cast<const DataInPin&>(second).info().type;
                const auto* output = static_cast<const DataOutPin&>(first).info().type;
                const bool accepts_type = meta::canInitialize(input, output);
                return accepts_type ? ELinkError::SUCCESS : ELinkError::UNMATCHED;
            }
            default:
                return ELinkError::WRONG_KIND;
            }
        }
    } // namespace

    FlowGraph::FlowGraph() = default;

    FlowGraph::~FlowGraph()
    {
        for (auto& [stored_id, node] : nodes_)
        {
            if (node)
            {
                node->graph_ = nullptr;
            }
        }
    }

    FlowGraph::FlowGraph(FlowGraph&& other) noexcept
        : variables_(std::move(other.variables_)), exports_(std::move(other.exports_)),
          next_var_id_(other.next_var_id_), nodes_(std::move(other.nodes_)), node_ids_(std::move(other.node_ids_)),
          pin_store_(std::move(other.pin_store_)), pin_ids_(std::move(other.pin_ids_)),
          topology_(std::move(other.topology_)), layout_(std::move(other.layout_))
    {
        rebindNodes();
    }

    FlowGraph& FlowGraph::operator=(FlowGraph&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        for (auto& [stored_id, node] : nodes_)
        {
            if (node)
            {
                node->graph_ = nullptr;
            }
        }
        variables_ = std::move(other.variables_);
        exports_ = std::move(other.exports_);
        next_var_id_ = other.next_var_id_;
        nodes_ = std::move(other.nodes_);
        node_ids_ = std::move(other.node_ids_);
        pin_store_ = std::move(other.pin_store_);
        pin_ids_ = std::move(other.pin_ids_);
        topology_ = std::move(other.topology_);
        layout_ = std::move(other.layout_);
        rebindNodes();
        return *this;
    }

    NodeId FlowGraph::addNode(std::unique_ptr<Node> node) noexcept
    {
        const bool is_invalid_candidate = !node || node->graph() != nullptr;
        if (is_invalid_candidate)
        {
            return {};
        }
        auto id = topology_.addNode(nodeType(*node));
        if (!id)
        {
            return {};
        }
        node_ids_.emplace(node.get(), *id);
        if (!attachNodeStructure(*id, *node, {}))
        {
            node_ids_.erase(node.get());
            return {};
        }
        nodes_.emplace(*id, std::move(node));
        return *id;
    }

    bool FlowGraph::insertNode(FlowNodeSnapshot snapshot) noexcept
    {
        auto& [id, node, pins] = snapshot;
        const bool is_invalid_candidate = !node || node->graph() != nullptr || !id.valid();
        if (is_invalid_candidate)
        {
            return false;
        }
        if (!topology_.insertNode({id, nodeType(*node)}))
        {
            return false;
        }
        node_ids_.emplace(node.get(), id);
        if (!attachNodeStructure(id, *node, pins))
        {
            node_ids_.erase(node.get());
            return false;
        }
        nodes_.emplace(id, std::move(node));
        return true;
    }

    Node* FlowGraph::findNodeById(NodeId id) noexcept
    {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : found->second.get();
    }

    const Node* FlowGraph::findNodeById(NodeId id) const noexcept
    {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : found->second.get();
    }

    NodeId FlowGraph::nodeId(const Node* node) const noexcept
    {
        const auto found = node_ids_.find(node);
        return found == node_ids_.end() ? NodeId{} : found->second;
    }

    bool FlowGraph::removeNode(NodeId id) noexcept
    {
        auto removed = extractNode(id);
        if (!removed)
        {
            return false;
        }
        static_cast<void>(layout_.erase(id));
        return true;
    }

    std::optional<FlowNodeSnapshot> FlowGraph::extractNode(NodeId id) noexcept
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
        {
            return std::nullopt;
        }
        auto pins = snapshotPins(*found->second);
        if (!topology_.detachNode(id))
        {
            return std::nullopt;
        }
        forgetPins(*found->second);
        auto node = std::move(found->second);
        node->graph_ = nullptr;
        node_ids_.erase(node.get());
        nodes_.erase(found);
        return FlowNodeSnapshot{id, std::move(node), std::move(pins)};
    }

    PinId FlowGraph::pinId(const Pin* pin) const noexcept
    {
        const auto found = pin_ids_.find(pin);
        return found == pin_ids_.end() ? PinId{} : found->second;
    }

    Pin* FlowGraph::findPin(PinId id) noexcept
    {
        const auto found = pin_store_.find(id);
        return found == pin_store_.end() ? nullptr : found->second;
    }

    const Pin* FlowGraph::findPin(PinId id) const noexcept
    {
        const auto found = pin_store_.find(id);
        return found == pin_store_.end() ? nullptr : found->second;
    }

    std::vector<Pin*> FlowGraph::linkedPins(PinId id)
    {
        std::vector<Pin*> result;
        for (const auto& link : topology_.links())
        {
            if (link.from == id)
            {
                result.push_back(findPin(link.to));
            }
            else if (link.to == id)
            {
                result.push_back(findPin(link.from));
            }
        }
        std::erase(result, nullptr);
        return result;
    }

    std::vector<const Pin*> FlowGraph::linkedPins(PinId id) const
    {
        std::vector<const Pin*> result;
        for (const auto& link : topology_.links())
        {
            if (link.from == id)
            {
                result.push_back(findPin(link.to));
            }
            else if (link.to == id)
            {
                result.push_back(findPin(link.from));
            }
        }
        std::erase(result, nullptr);
        return result;
    }

    ELinkError FlowGraph::connect(const Pin& first, const Pin& second) noexcept
    {
        const auto first_id = pinId(&first);
        const auto second_id = pinId(&second);
        const auto* first_record = topology_.findPin(first_id);
        const auto* second_record = topology_.findPin(second_id);
        const bool has_foreign_pin = first_record == nullptr || second_record == nullptr;
        if (has_foreign_pin)
        {
            return ELinkError::INVALID_PIN;
        }
        if (first_record->owner == second_record->owner)
        {
            return ELinkError::SAME_NODE;
        }
        bool first_linked{};
        bool second_linked{};
        bool exact_link{};
        for (const auto& link : topology_.links())
        {
            const bool touches_first = link.from == first_id || link.to == first_id;
            const bool touches_second = link.from == second_id || link.to == second_id;
            first_linked |= touches_first;
            second_linked |= touches_second;
            exact_link |= touches_first && touches_second;
        }
        const auto first_preflight = pinConnection(first, second, first_linked, exact_link);
        if (first_preflight != ELinkError::SUCCESS)
        {
            return first_preflight;
        }
        const auto second_preflight = pinConnection(second, first, second_linked, exact_link);
        if (second_preflight != ELinkError::SUCCESS)
        {
            return second_preflight;
        }
        const auto from = first_record->direction == lux::graph::EPinDirection::OUTPUT ? first_id : second_id;
        const auto to = first_record->direction == lux::graph::EPinDirection::INPUT ? first_id : second_id;
        const auto connected = topology_.connect(from, to);
        if (connected)
        {
            return ELinkError::SUCCESS;
        }
        const bool has_conflicting_link = connected.error().code == lux::graph::EGraphTopologyError::DUPLICATE_LINK ||
                                          connected.error().code == lux::graph::EGraphTopologyError::FAN_CAP_EXCEEDED;
        if (has_conflicting_link)
        {
            return ELinkError::HAS_LINKED;
        }
        return ELinkError::WRONG_KIND;
    }

    ELinkError FlowGraph::disconnect(const Pin& first, const Pin& second) noexcept
    {
        const auto first_id = pinId(&first);
        const auto second_id = pinId(&second);
        const auto* first_record = topology_.findPin(first_id);
        const auto* second_record = topology_.findPin(second_id);
        const bool has_foreign_pin = first_record == nullptr || second_record == nullptr;
        if (has_foreign_pin)
        {
            return ELinkError::INVALID_PIN;
        }
        const auto from = first_record->direction == lux::graph::EPinDirection::OUTPUT ? first_id : second_id;
        const auto to = first_record->direction == lux::graph::EPinDirection::INPUT ? first_id : second_id;
        return topology_.disconnect(from, to) ? ELinkError::UNLINKED : ELinkError::UNMATCHED;
    }

    bool FlowGraph::registerPin(Pin& pin, PinId restored) noexcept
    {
        const auto owner = nodeId(pin.node());
        const bool is_invalid_pin = !owner.valid() || pin_ids_.contains(&pin);
        if (is_invalid_pin)
        {
            return false;
        }
        const auto direction =
            isInput(pin.kind()) ? lux::graph::EPinDirection::INPUT : lux::graph::EPinDirection::OUTPUT;
        auto id = restored;
        if (id.valid())
        {
            if (!topology_.insertPin({id, owner, direction, fanCap(pin.kind()), pinSemantic(pin)}))
            {
                return false;
            }
        }
        else
        {
            auto created = topology_.addPin(owner, direction, fanCap(pin.kind()), pinSemantic(pin));
            if (!created)
            {
                return false;
            }
            id = *created;
        }
        pin_store_.emplace(id, &pin);
        pin_ids_.emplace(&pin, id);
        return true;
    }

    void FlowGraph::unregisterPin(Pin& pin) noexcept
    {
        const auto id = pinId(&pin);
        if (id.valid())
        {
            static_cast<void>(topology_.detachPin(id));
            pin_store_.erase(id);
            pin_ids_.erase(&pin);
        }
    }

    bool FlowGraph::assignPinId(Pin& pin, PinId id) noexcept
    {
        const bool is_invalid_id = !id.valid() || topology_.findPin(id) != nullptr;
        const bool is_foreign_pin = !pin.node() || pin.node()->graph() != this;
        if (is_invalid_id || is_foreign_pin)
        {
            return false;
        }
        unregisterPin(pin);
        return registerPin(pin, id);
    }

    std::vector<PinId> FlowGraph::snapshotPins(const Node& node) const
    {
        std::vector<PinId> result;
        result.reserve(node.inPins().size() + node.outPins().size());
        for (const bool input : {true, false})
        {
            for (const auto* pin : input ? node.inPins() : node.outPins())
            {
                result.push_back(pinId(pin));
            }
        }
        return result;
    }

    void FlowGraph::forgetPins(const Node& node) noexcept
    {
        for (const bool input : {true, false})
        {
            for (const auto* pin : input ? node.inPins() : node.outPins())
            {
                pin_store_.erase(pinId(pin));
                pin_ids_.erase(pin);
            }
        }
    }

    bool FlowGraph::attachNodeStructure(NodeId id, Node& node, std::span<const PinId> restored) noexcept
    {
        const auto count = node.inPins().size() + node.outPins().size();
        if (!restored.empty() && restored.size() != count)
        {
            static_cast<void>(topology_.detachNode(id));
            return false;
        }
        node.graph_ = this;
        // Restore all issued identities before allocating new signature pins.
        for (const bool fresh : {false, true})
        {
            std::size_t ordinal{};
            for (const bool input : {true, false})
            {
                for (auto* pin : input ? node.inPins() : node.outPins())
                {
                    const auto saved = restored.empty() ? PinId{} : restored[ordinal];
                    ++ordinal;
                    if (fresh == saved.valid())
                    {
                        continue;
                    }
                    const bool is_invalid_pin = !pin || pin->node() != &node || isInput(pin->kind()) != input;
                    if (is_invalid_pin || !registerPin(*pin, saved))
                    {
                        forgetPins(node);
                        node.graph_ = nullptr;
                        static_cast<void>(topology_.detachNode(id));
                        return false;
                    }
                }
            }
        }
        return true;
    }

    void FlowGraph::rebindNodes() noexcept
    {
        for (auto& [stored_id, node] : nodes_)
        {
            if (node)
            {
                node->graph_ = this;
            }
        }
    }
} // namespace lux::flowforge

namespace lux::flowforge
{
    FlowGraphEdit::FlowGraphEdit(FlowGraph& target) : target_(&target), structure_(target.topology_, target.layout_) {}

    FlowGraphEdit::~FlowGraphEdit() = default;

    FlowGraphEdit::FlowGraphEdit(FlowGraphEdit&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)), structure_(std::move(other.structure_)),
          nodes_(std::move(other.nodes_)), node_ids_(std::move(other.node_ids_)),
          pin_store_(std::move(other.pin_store_)), pin_ids_(std::move(other.pin_ids_)),
          insert_(std::move(other.insert_)), pins_(std::move(other.pins_)),
          inserted_ids_(std::move(other.inserted_ids_)), keep_(std::move(other.keep_)), erase_(std::move(other.erase_)),
          removed_(std::move(other.removed_)), storage_changed_(other.storage_changed_)
    {
    }

    FlowGraphEdit::Result FlowGraphEdit::prepare(FlowGraph& graph, const FlowGraphChange& change)
    {
        using E = lux::graph::EGraphTopologyError;
        const auto failure = [](E error, NodeId node = {}, PinId pin = {})
        { return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{error, node, pin}); };
        FlowGraphEdit plan(graph);
        plan.storage_changed_ = !change.insert.empty() || !change.erase.empty();
        for (const auto id : change.erase)
        {
            const auto* node = graph.findNodeById(id);
            if (!node)
            {
                return failure(E::UNKNOWN_NODE, id);
            }
            // Removing a referenced definition requires removing its users in the same batch.
            for (const auto& storage : graph.nodes())
            {
                if (std::ranges::find(change.erase, storage.id) != change.erase.end())
                {
                    continue;
                }
                const auto* user = storage.node;
                if ((user->operation() == ENodeOperation::GRAPH_FUNC_CALL &&
                     static_cast<const GraphFuncCallNode*>(user)->calleeId() == id) ||
                    (user->operation() == ENodeOperation::FUNC_RETURN &&
                     static_cast<const FuncReturnNode*>(user)->definitionId() == id))
                {
                    return failure(E::INVALID_ID, id);
                }
            }
            for (const auto& exported : graph.exports())
            {
                if (exported.entry_node_id == id)
                {
                    const bool replaced_entry = std::ranges::any_of(
                        change.insert,
                        [&](const auto& entry)
                        {
                            return entry.node && *entry.node && entry.id == id &&
                                   (*entry.node)->operation() == ENodeOperation::ON_EVENT;
                        }
                    );
                    if (!replaced_entry)
                    {
                        return failure(E::INVALID_ID, id);
                    }
                }
            }
        }
        for (const auto& link : change.disconnect)
        {
            auto result = plan.structure_.disconnect(link.from, link.to);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
        }
        for (const auto id : change.erase)
        {
            auto removed = plan.structure_.detachNode(id);
            if (!removed)
            {
                return lux::cxx::unexpected(removed.error());
            }
        }
        if (plan.storage_changed_)
        {
            plan.node_ids_.reserve(graph.nodes().size() + change.insert.size());
            for (const auto& storage : graph.nodes())
            {
                if (std::ranges::find(change.erase, storage.id) != change.erase.end())
                {
                    plan.erase_.push_back(storage.id);
                    plan.removed_.push_back({storage.id, nullptr, graph.snapshotPins(*storage.node)});
                }
                else
                {
                    plan.nodes_.emplace(storage.id, nullptr);
                    plan.node_ids_.emplace(storage.node, storage.id);
                    plan.keep_.push_back(storage.id);
                    for (const bool input : {true, false})
                    {
                        for (auto* pin : input ? storage.node->inPins() : storage.node->outPins())
                        {
                            const auto pin_id = graph.pinId(pin);
                            plan.pin_store_.emplace(pin_id, pin);
                            plan.pin_ids_.emplace(pin, pin_id);
                        }
                    }
                }
            }
        }
        for (const auto& candidate : change.insert)
        {
            auto* source = candidate.node;
            if (!source || !*source || (*source)->graph())
            {
                return failure(E::INVALID_ID);
            }
            if (std::ranges::find(plan.insert_, source, &Insertion::source) != plan.insert_.end())
            {
                return failure(E::DUPLICATE_NODE, candidate.id);
            }
            auto& node = **source;
            const auto valid_reference = [&](const auto& reference, NodeId definition_id)
            {
                const Node* definition = nullptr;
                for (const auto& insertion : change.insert)
                {
                    const bool is_definition_candidate =
                        definition_id.valid() && insertion.id == definition_id && insertion.node;
                    if (is_definition_candidate)
                    {
                        definition = insertion.node->get();
                        break;
                    }
                }
                if (!definition && std::ranges::find(change.erase, definition_id) == change.erase.end())
                {
                    definition = graph.findNodeById(definition_id);
                }
                const bool is_definition = definition && definition->operation() == ENodeOperation::FUNC_DEF_START;
                return is_definition && reference.matchesSignature(static_cast<const FuncDefNode&>(*definition));
            };
            const bool is_invalid_call = node.operation() == ENodeOperation::GRAPH_FUNC_CALL &&
                                         !valid_reference(
                                             static_cast<const GraphFuncCallNode&>(node),
                                             static_cast<const GraphFuncCallNode&>(node).calleeId()
                                         );
            const bool is_invalid_return = node.operation() == ENodeOperation::FUNC_RETURN &&
                                           !valid_reference(
                                               static_cast<const FuncReturnNode&>(node),
                                               static_cast<const FuncReturnNode&>(node).definitionId()
                                           );
            if (is_invalid_call || is_invalid_return)
            {
                return failure(E::INVALID_ID, candidate.id);
            }
            if (node.operation() == ENodeOperation::GET_VARIABLE || node.operation() == ENodeOperation::SET_VARIABLE)
            {
                const auto variable_id = node.operation() == ENodeOperation::GET_VARIABLE
                                             ? static_cast<const GetVariableNode&>(node).variableId()
                                             : static_cast<const SetVariableNode&>(node).variableId();
                const auto* variable = graph.findVariable(variable_id);
                if (!variable)
                {
                    return failure(E::INVALID_ID, candidate.id);
                }
                for (const auto* pin : node.outPins())
                {
                    if (pin->kind() == EPinKind::DATA_OUT)
                    {
                        const auto* type = static_cast<const DataOutPin*>(pin)->info().type;
                        if (!type || !variable->type || *type != *variable->type)
                        {
                            return failure(E::INVALID_ID, candidate.id);
                        }
                    }
                }
                for (const auto* pin : node.inPins())
                {
                    if (pin->kind() == EPinKind::DATA_IN)
                    {
                        const auto* type = static_cast<const DataInPin*>(pin)->info().type;
                        if (!type || !variable->type || *type != *variable->type)
                        {
                            return failure(E::INVALID_ID, candidate.id);
                        }
                    }
                }
            }

            auto id = candidate.id;
            const bool restoring = id.valid();
            if (restoring)
            {
                auto result = plan.structure_.insertNode({id, nodeType(node)});
                if (!result)
                {
                    return lux::cxx::unexpected(result.error());
                }
            }
            else
            {
                auto result = plan.structure_.addNode(nodeType(node));
                if (!result)
                {
                    return lux::cxx::unexpected(result.error());
                }
                id = *result;
            }
            const auto pin_count = node.inPins().size() + node.outPins().size();
            const bool has_invalid_snapshot =
                !candidate.pins.empty() && (!restoring || candidate.pins.size() != pin_count);
            if (has_invalid_snapshot)
            {
                return failure(E::INVALID_ID, id);
            }
            std::size_t ordinal{};
            for (const bool input : {true, false})
            {
                const auto& pins = input ? node.inPins() : node.outPins();
                for (const auto* pin : pins)
                {
                    if (!pin || pin->node() != &node || isInput(pin->kind()) != input)
                    {
                        return failure(E::INVALID_DIRECTION, id);
                    }
                    const auto direction = input ? lux::graph::EPinDirection::INPUT : lux::graph::EPinDirection::OUTPUT;
                    auto pin_id = candidate.pins.empty() ? PinId{} : candidate.pins[ordinal];
                    ++ordinal;
                    if (restoring && !pin_id.valid())
                    {
                        // Allocate new signature pins after every preserved ID has been registered.
                        plan.pins_.emplace_back(const_cast<Pin*>(pin), PinId{});
                        continue;
                    }
                    if (restoring)
                    {
                        auto result =
                            plan.structure_.insertPin({pin_id, id, direction, fanCap(pin->kind()), pinSemantic(*pin)});
                        if (!result)
                        {
                            return lux::cxx::unexpected(result.error());
                        }
                    }
                    else
                    {
                        auto result = plan.structure_.addPin(id, direction, fanCap(pin->kind()), pinSemantic(*pin));
                        if (!result)
                        {
                            return lux::cxx::unexpected(result.error());
                        }
                        pin_id = *result;
                    }
                    plan.pins_.emplace_back(const_cast<Pin*>(pin), pin_id);
                }
            }
            plan.nodes_.emplace(id, nullptr);
            plan.node_ids_.emplace(source->get(), id);
            plan.insert_.push_back({source, id});
            plan.inserted_ids_.push_back(id);
        }
        for (auto& [pin, assigned] : plan.pins_)
        {
            if (!assigned.valid())
            {
                const auto insertion = std::ranges::find_if(
                    plan.insert_,
                    [&](const auto& entry) { return entry.source->get() == pin->node(); }
                );
                const auto direction =
                    isInput(pin->kind()) ? lux::graph::EPinDirection::INPUT : lux::graph::EPinDirection::OUTPUT;
                auto result = plan.structure_.addPin(insertion->id, direction, fanCap(pin->kind()), pinSemantic(*pin));
                if (!result)
                {
                    return lux::cxx::unexpected(result.error());
                }
                assigned = *result;
            }
        }
        for (const auto& [pin, id] : plan.pins_)
        {
            plan.pin_store_.emplace(id, pin);
            plan.pin_ids_.emplace(pin, id);
        }
        const auto pin_at = [&](PinId id) -> const Pin*
        {
            const auto& store = plan.storage_changed_ ? plan.pin_store_ : graph.pin_store_;
            const auto found = store.find(id);
            return found == store.end() ? nullptr : found->second;
        };
        for (const auto& link : change.connect)
        {
            const auto* from = pin_at(link.from);
            const auto* to = pin_at(link.to);
            const bool has_payload_pins = from != nullptr && to != nullptr;
            const bool has_structural_pins = plan.structure_.topology().findPin(link.from) != nullptr &&
                                             plan.structure_.topology().findPin(link.to) != nullptr;
            const bool has_missing_pin = !has_payload_pins || !has_structural_pins;
            if (has_missing_pin)
            {
                return failure(E::UNKNOWN_PIN, {}, !from ? link.from : link.to);
            }
            const bool exec = from->kind() == EPinKind::EXEC_OUT && to->kind() == EPinKind::EXEC_IN;
            const bool data = from->kind() == EPinKind::DATA_OUT && to->kind() == EPinKind::DATA_IN;
            if ((!exec && !data) || from->node() == to->node())
            {
                return failure(E::DIRECTION_MISMATCH, {}, link.to);
            }
            if (data)
            {
                const auto* output = static_cast<const DataOutPin*>(from)->info().type;
                const auto* input = static_cast<const DataInPin*>(to)->info().type;
                if (!input || !output || !lux::meta::canInitialize(input, output))
                {
                    return failure(E::INVALID_TYPE, {}, link.to);
                }
            }
            auto result = plan.structure_.connect(link.from, link.to);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
        }
        for (const auto& entry : change.place)
        {
            auto result = plan.place(entry.node, entry.layout);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
        }
        for (const auto id : change.unplace)
        {
            auto result = plan.structure_.unplace(id);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
        }
        return plan;
    }

    std::span<const NodeId> FlowGraphEdit::insertedIds() const noexcept
    {
        return inserted_ids_;
    }

    std::span<const std::pair<Pin*, PinId>> FlowGraphEdit::assignedPins() const noexcept
    {
        return pins_;
    }

    lux::cxx::expected<void, lux::graph::GraphTopologyFailure> FlowGraphEdit::place(
        NodeId id,
        lux::graph::GraphNodeLayout value
    )
    {
        if (!target_)
        {
            return lux::cxx::unexpected(
                lux::graph::GraphTopologyFailure{lux::graph::EGraphTopologyError::INVALID_ID, id}
            );
        }
        return structure_.place(id, value);
    }

    void FlowGraphEdit::commit() noexcept
    {
        if (!target_)
        {
            std::terminate();
        }
        auto& graph = *target_;
        for (std::size_t i = 0; i < erase_.size(); ++i)
        {
            auto& node = graph.nodes_.at(erase_[i]);
            node->graph_ = nullptr;
            removed_[i].node = std::move(node);
        }
        for (const auto index : keep_)
        {
            nodes_.at(index) = std::move(graph.nodes_.at(index));
        }
        for (const auto& insertion : insert_)
        {
            auto& node = *insertion.source;
            node->graph_ = &graph;
            nodes_.at(insertion.id) = std::move(node);
        }
        if (storage_changed_)
        {
            std::swap(graph.nodes_, nodes_);
            std::swap(graph.node_ids_, node_ids_);
            std::swap(graph.pin_store_, pin_store_);
            std::swap(graph.pin_ids_, pin_ids_);
        }
        structure_.commit();
        target_ = nullptr;
    }

    std::vector<FlowNodeSnapshot> FlowGraphEdit::takeRemoved() noexcept
    {
        if (target_)
        {
            std::terminate();
        }
        return std::move(removed_);
    }
} // namespace lux::flowforge
