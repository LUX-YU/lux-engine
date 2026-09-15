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
[[nodiscard]] lux::graph::NodeTypeId nodeType(ENodeOperation operation) noexcept
{
    return lux::graph::NodeTypeId{static_cast<std::uint64_t>(operation) + 1U};
}

[[nodiscard]] bool isInput(EPinKind kind) noexcept
{
    return kind == EPinKind::EXEC_IN || kind == EPinKind::DATA_IN;
}

[[nodiscard]] std::uint8_t fanCap(EPinKind kind) noexcept
{
    return kind == EPinKind::EXEC_OUT || kind == EPinKind::DATA_IN ? 1U : lux::graph::kUnlimitedFan;
}

[[nodiscard]] lux::graph::PinSemanticId pinSemantic(const Pin &pin) noexcept
{
    const auto kind = static_cast<std::uint64_t>(pin.kind()) + 1U;
    const auto &pins = isInput(pin.kind()) ? pin.node()->inPins() : pin.node()->outPins();
    const auto found = std::ranges::find(pins, std::addressof(pin));
    const auto ordinal = found == pins.end() ? 0U : static_cast<std::uint64_t>(found - pins.begin()) + 1U;
    return lux::graph::PinSemanticId{(kind << 56U) | ordinal};
}
} // namespace

FlowGraph::FlowGraph() = default;

bool FlowGraph::assignDetachedPinId(Pin &pin, PinId id) noexcept
{
    if (!id.valid() || !pin.node() || pin.node()->graph())
    {
        return false;
    }
    pin.setId(id);
    return true;
}

FlowGraph::~FlowGraph()
{
    for (auto &storage : nodes_.values())
    {
        if (storage.node)
        {
            storage.node->assignGraph(nullptr);
        }
    }
}

FlowGraph::FlowGraph(FlowGraph &&other) noexcept
    : variables_(std::move(other.variables_)), exports_(std::move(other.exports_)), next_var_id_(other.next_var_id_),
      nodes_(std::move(other.nodes_)), topology_(std::move(other.topology_)), layout_(std::move(other.layout_))
{
    rebindNodes();
}

FlowGraph &FlowGraph::operator=(FlowGraph &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    for (auto &storage : nodes_.values())
    {
        if (storage.node)
        {
            storage.node->assignGraph(nullptr);
        }
    }
    variables_ = std::move(other.variables_);
    exports_ = std::move(other.exports_);
    next_var_id_ = other.next_var_id_;
    nodes_ = std::move(other.nodes_);
    topology_ = std::move(other.topology_);
    layout_ = std::move(other.layout_);
    rebindNodes();
    return *this;
}

const std::vector<NodeStorage> &FlowGraph::nodes() const
{
    return nodes_.values();
}

size_t FlowGraph::addNodes(std::unique_ptr<Node> node)
{
    if (!node)
    {
        return (std::numeric_limits<std::size_t>::max)();
    }
    auto id = topology_.addNode(nodeType(node->operation()));
    if (!id)
    {
        return (std::numeric_limits<std::size_t>::max)();
    }
    node->assignStableId(*id);
    if (!attachNodeStructure(*node, false))
    {
        return (std::numeric_limits<std::size_t>::max)();
    }
    try
    {
        const auto index = nodes_.emplace(std::move(node), 0U);
        nodes_.at(index).index = index;
        return index;
    }
    catch (...)
    {
        static_cast<void>(topology_.detachNode(*id));
        return (std::numeric_limits<std::size_t>::max)();
    }
}

size_t FlowGraph::addNodesWithId(NodeId stable_id, std::unique_ptr<Node> node)
{
    if (!node || !stable_id.valid())
    {
        return (std::numeric_limits<std::size_t>::max)();
    }
    if (!topology_.insertNode(lux::graph::NodeRecord{stable_id, nodeType(node->operation())}))
    {
        return (std::numeric_limits<std::size_t>::max)();
    }
    node->assignStableId(stable_id);
    const auto preserve_pin_ids =
        std::ranges::all_of(node->inPins(), [](const Pin *pin) { return pin != nullptr && pin->id().valid(); }) &&
        std::ranges::all_of(node->outPins(), [](const Pin *pin) { return pin != nullptr && pin->id().valid(); });
    if (!attachNodeStructure(*node, preserve_pin_ids))
    {
        return (std::numeric_limits<std::size_t>::max)();
    }
    try
    {
        const auto index = nodes_.emplace(std::move(node), 0U);
        nodes_.at(index).index = index;
        return index;
    }
    catch (...)
    {
        static_cast<void>(topology_.detachNode(stable_id));
        return (std::numeric_limits<std::size_t>::max)();
    }
}

Node *FlowGraph::findNodeById(NodeId stable_id)
{
    for (auto &storage : nodes_.values())
    {
        if (storage.node && storage.node->id() == stable_id)
        {
            return storage.node.get();
        }
    }
    return nullptr;
}

const Node *FlowGraph::findNodeById(NodeId stable_id) const
{
    for (const auto &storage : nodes_.values())
    {
        if (storage.node && storage.node->id() == stable_id)
        {
            return storage.node.get();
        }
    }
    return nullptr;
}

bool FlowGraph::removeNode(size_t index)
{
    if (!nodes_.contains(index))
    {
        return false;
    }
    auto &node = *nodes_.at(index).node;
    if (!topology_.detachNode(node.id()))
    {
        return false;
    }
    static_cast<void>(layout_.erase(node.id()));
    node.assignGraph(nullptr);
    nodes_.erase(index);
    return true;
}

bool FlowGraph::extractNode(size_t index, NodeStorage &out)
{
    if (!nodes_.contains(index))
    {
        return false;
    }
    auto &node = *nodes_.at(index).node;
    if (!topology_.detachNode(node.id()))
    {
        return false;
    }
    node.assignGraph(nullptr);
    return nodes_.extract(index, out);
}

bool FlowGraph::insertNodeAt(size_t index, std::unique_ptr<Node> node)
{
    if (!node || !node->id().valid() || nodes_.contains(index))
    {
        return false;
    }
    const auto id = node->id();
    if (!topology_.insertNode(lux::graph::NodeRecord{id, nodeType(node->operation())}))
    {
        return false;
    }
    if (!attachNodeStructure(*node, true))
    {
        return false;
    }
    if (!nodes_.try_emplace_at(index, std::move(node), 0U))
    {
        static_cast<void>(topology_.detachNode(id));
        return false;
    }
    nodes_.at(index).index = index;
    return true;
}

Pin *FlowGraph::findPin(PinId id) noexcept
{
    for (auto &storage : nodes_.values())
    {
        if (!storage.node)
        {
            continue;
        }
        for (auto *pin : storage.node->inPins())
        {
            if (pin != nullptr && pin->id() == id)
            {
                return pin;
            }
        }
        for (auto *pin : storage.node->outPins())
        {
            if (pin != nullptr && pin->id() == id)
            {
                return pin;
            }
        }
    }
    return nullptr;
}

const Pin *FlowGraph::findPin(PinId id) const noexcept
{
    return const_cast<FlowGraph *>(this)->findPin(id);
}

std::vector<Pin *> FlowGraph::linkedPins(PinId id)
{
    std::vector<Pin *> result;
    for (const auto &link : topology_.links())
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

std::vector<const Pin *> FlowGraph::linkedPins(PinId id) const
{
    std::vector<const Pin *> result;
    for (const auto &link : topology_.links())
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

ELinkError FlowGraph::connect(Pin &first, Pin &second) noexcept
{
    const auto first_preflight = first.canLink(std::addressof(second));
    if (first_preflight != ELinkError::SUCCESS)
    {
        return first_preflight;
    }
    const auto second_preflight = second.canLink(std::addressof(first));
    if (second_preflight != ELinkError::SUCCESS)
    {
        return second_preflight;
    }

    auto *first_record = topology_.findPin(first.id());
    auto *second_record = topology_.findPin(second.id());
    if (first_record == nullptr || second_record == nullptr)
    {
        return ELinkError::INVALID_PIN;
    }
    const auto from = first_record->direction == lux::graph::EPinDirection::OUTPUT ? first.id() : second.id();
    const auto to = first_record->direction == lux::graph::EPinDirection::INPUT ? first.id() : second.id();
    const auto connected = topology_.connect(from, to);
    if (connected)
    {
        return ELinkError::SUCCESS;
    }
    if (connected.error().code == lux::graph::EGraphTopologyError::DUPLICATE_LINK ||
        connected.error().code == lux::graph::EGraphTopologyError::FAN_CAP_EXCEEDED)
    {
        return ELinkError::HAS_LINKED;
    }
    return ELinkError::WRONG_KIND;
}

ELinkError FlowGraph::disconnect(Pin &first, Pin &second) noexcept
{
    const auto *first_record = topology_.findPin(first.id());
    const auto *second_record = topology_.findPin(second.id());
    if (first_record == nullptr || second_record == nullptr)
    {
        return ELinkError::INVALID_PIN;
    }
    const auto from = first_record->direction == lux::graph::EPinDirection::OUTPUT ? first.id() : second.id();
    const auto to = first_record->direction == lux::graph::EPinDirection::INPUT ? first.id() : second.id();
    return topology_.disconnect(from, to) ? ELinkError::UNLINKED : ELinkError::UNMATCHED;
}

bool FlowGraph::registerPin(Pin &pin) noexcept
{
    const auto direction = isInput(pin.kind()) ? lux::graph::EPinDirection::INPUT : lux::graph::EPinDirection::OUTPUT;
    if (const auto *existing = topology_.findPin(pin.id()); existing != nullptr)
    {
        return existing->owner == pin.node()->id() && existing->direction == direction &&
               existing->semantic == pinSemantic(pin);
    }
    if (pin.id().valid())
    {
        return static_cast<bool>(topology_.insertPin(
            lux::graph::PinRecord{pin.id(), pin.node()->id(), direction, fanCap(pin.kind()), pinSemantic(pin)}));
    }
    auto created = topology_.addPin(pin.node()->id(), direction, fanCap(pin.kind()), pinSemantic(pin));
    if (!created)
    {
        return false;
    }
    pin.setId(*created);
    return true;
}

void FlowGraph::unregisterPin(Pin &pin) noexcept
{
    if (pin.id().valid())
    {
        static_cast<void>(topology_.detachPin(pin.id()));
    }
}

bool FlowGraph::assignPinId(Pin &pin, PinId id) noexcept
{
    if (!id.valid() || topology_.findPin(id) != nullptr)
    {
        return false;
    }
    unregisterPin(pin);
    pin.setId(id);
    if (registerPin(pin))
    {
        return true;
    }
    pin.setId({});
    return false;
}

bool FlowGraph::attachNodeStructure(Node &node, bool preserve_pin_ids) noexcept
{
    if (!preserve_pin_ids)
    {
        for (auto *pin : node.inPins())
        {
            if (pin != nullptr)
            {
                pin->setId({});
            }
        }
        for (auto *pin : node.outPins())
        {
            if (pin != nullptr)
            {
                pin->setId({});
            }
        }
    }
    node.assignGraph(this);
    for (auto *pin : node.inPins())
    {
        if (pin == nullptr || !registerPin(*pin))
        {
            node.assignGraph(nullptr);
            static_cast<void>(topology_.detachNode(node.id()));
            return false;
        }
    }
    for (auto *pin : node.outPins())
    {
        if (pin == nullptr || !registerPin(*pin))
        {
            node.assignGraph(nullptr);
            static_cast<void>(topology_.detachNode(node.id()));
            return false;
        }
    }
    return true;
}

void FlowGraph::rebindNodes() noexcept
{
    for (auto &storage : nodes_.values())
    {
        if (storage.node)
        {
            storage.node->assignGraph(this);
        }
    }
}
} // namespace lux::flowforge

namespace lux::flowforge
{
FlowGraphEdit::FlowGraphEdit(FlowGraph &target) : target_(&target) {}
FlowGraphEdit::~FlowGraphEdit() = default;
FlowGraphEdit::FlowGraphEdit(FlowGraphEdit &&other) noexcept
    : target_(std::exchange(other.target_, nullptr)), topology_(std::move(other.topology_)),
      layout_(std::move(other.layout_)), nodes_(std::move(other.nodes_)), insert_(std::move(other.insert_)),
      pins_(std::move(other.pins_)), inserted_ids_(std::move(other.inserted_ids_)), keep_(std::move(other.keep_)),
      erase_(std::move(other.erase_)), removed_(std::move(other.removed_)), topology_changed_(other.topology_changed_),
      layout_changed_(other.layout_changed_), storage_changed_(other.storage_changed_)
{
}

FlowGraphEdit::Result FlowGraphEdit::prepare(FlowGraph &graph, const FlowGraphChange &change)
{
    using E = lux::graph::EGraphTopologyError;
    const auto failure = [](E error, NodeId node = {}, PinId pin = {}) {
        return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{error, node, pin});
    };
    FlowGraphEdit plan(graph);
    plan.storage_changed_ = !change.insert.empty() || !change.erase.empty();
    plan.topology_changed_ = plan.storage_changed_ || !change.connect.empty() || !change.disconnect.empty();
    plan.layout_changed_ = !change.erase.empty() || !change.place.empty() || !change.unplace.empty();
    if (plan.topology_changed_)
    {
        plan.topology_ = graph.topology_;
    }
    if (plan.layout_changed_)
    {
        plan.layout_ = graph.layout_;
    }
    const auto &final_topology = plan.topology_changed_ ? plan.topology_ : graph.topology_;
    for (const auto id : change.erase)
    {
        const auto *node = graph.findNodeById(id);
        if (!node)
        {
            return failure(E::UNKNOWN_NODE, id);
        }
        // A definition cannot disappear while one of its callers still holds its address.
        for (const auto &storage : graph.nodes())
        {
            if (std::ranges::find(change.erase, storage.node->id()) != change.erase.end())
            {
                continue;
            }
            const auto *user = storage.node.get();
            if ((user->operation() == ENodeOperation::GRAPH_FUNC_CALL &&
                 static_cast<const GraphFuncCallNode *>(user)->callee() == node) ||
                (user->operation() == ENodeOperation::FUNC_RETURN &&
                 static_cast<const FuncReturnNode *>(user)->def() == node))
            {
                return failure(E::INVALID_ID, id);
            }
        }
        for (const auto &exported : graph.exports())
        {
            if (exported.entry_node_id == id)
            {
                const bool replaced_entry = std::ranges::any_of(change.insert, [&](const auto *entry) {
                    return entry && *entry && (*entry)->id() == id && (*entry)->operation() == ENodeOperation::ON_EVENT;
                });
                if (!replaced_entry)
                {
                    return failure(E::INVALID_ID, id);
                }
            }
        }
    }
    for (const auto &link : change.disconnect)
    {
        auto result = plan.topology_.disconnect(link.from, link.to);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
    }
    for (const auto id : change.erase)
    {
        auto removed = plan.topology_.detachNode(id);
        if (!removed)
        {
            return lux::cxx::unexpected(removed.error());
        }
        static_cast<void>(plan.layout_.erase(id));
    }
    if (plan.storage_changed_)
    {
        plan.nodes_.reserve(graph.nodes().size() + change.insert.size());
        for (const auto &storage : graph.nodes())
        {
            if (!plan.nodes_.try_emplace_at(storage.index, std::unique_ptr<Node>{}, storage.index))
            {
                return failure(E::DUPLICATE_NODE, storage.node->id());
            }
            if (std::ranges::find(change.erase, storage.node->id()) != change.erase.end())
            {
                plan.erase_.push_back(storage.index);
            }
            else
            {
                plan.keep_.push_back(storage.index);
            }
        }
        for (const auto index : plan.erase_)
        {
            plan.nodes_.erase(index);
        }
        plan.removed_.resize(plan.erase_.size());
    }
    for (auto *source : change.insert)
    {
        if (!source || !*source || (*source)->graph())
        {
            return failure(E::INVALID_ID);
        }
        if (std::ranges::find(plan.insert_, source, &Insertion::source) != plan.insert_.end())
        {
            return failure(E::DUPLICATE_NODE, (*source)->id());
        }
        auto &node = **source;
        const auto owns_definition = [&](const FuncDefNode *definition) {
            if (!definition)
            {
                return false;
            }
            for (const auto &storage : graph.nodes())
            {
                if (storage.node.get() == definition)
                {
                    return std::ranges::find(change.erase, storage.node->id()) == change.erase.end();
                }
            }
            return std::ranges::any_of(
                change.insert, [&](const auto *insertion) { return insertion && insertion->get() == definition; });
        };
        if ((node.operation() == ENodeOperation::GRAPH_FUNC_CALL &&
             !owns_definition(static_cast<const GraphFuncCallNode &>(node).callee())) ||
            (node.operation() == ENodeOperation::FUNC_RETURN &&
             !owns_definition(static_cast<const FuncReturnNode &>(node).def())))
        {
            return failure(E::INVALID_ID, node.id());
        }
        if (node.operation() == ENodeOperation::GET_VARIABLE || node.operation() == ENodeOperation::SET_VARIABLE)
        {
            const auto variable_id = node.operation() == ENodeOperation::GET_VARIABLE
                                         ? static_cast<const GetVariableNode &>(node).variableId()
                                         : static_cast<const SetVariableNode &>(node).variableId();
            const auto *variable = graph.findVariable(variable_id);
            if (!variable)
            {
                return failure(E::INVALID_ID, node.id());
            }
            for (const auto *pin : node.outPins())
            {
                if (pin->kind() == EPinKind::DATA_OUT)
                {
                    const auto *type = static_cast<const DataOutPin *>(pin)->info().type;
                    if (!type || !variable->type || *type != *variable->type)
                    {
                        return failure(E::INVALID_ID, node.id(), pin->id());
                    }
                }
            }
            for (const auto *pin : node.inPins())
            {
                if (pin->kind() == EPinKind::DATA_IN)
                {
                    const auto *type = static_cast<const DataInPin *>(pin)->info().type;
                    if (!type || !variable->type || *type != *variable->type)
                    {
                        return failure(E::INVALID_ID, node.id(), pin->id());
                    }
                }
            }
        }

        auto id = node.id();
        const bool restoring = change.preserve_insert_ids;
        if (restoring)
        {
            auto result = plan.topology_.insertNode({id, nodeType(node.operation())});
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
        }
        else
        {
            auto result = plan.topology_.addNode(nodeType(node.operation()));
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            id = *result;
        }
        for (const bool input : {true, false})
        {
            const auto &pins = input ? node.inPins() : node.outPins();
            for (const auto *pin : pins)
            {
                if (!pin || pin->node() != &node || isInput(pin->kind()) != input)
                {
                    return failure(E::INVALID_DIRECTION, id);
                }
                const auto direction = input ? lux::graph::EPinDirection::INPUT : lux::graph::EPinDirection::OUTPUT;
                auto pin_id = pin->id();
                if (restoring && !pin_id.valid())
                {
                    // Allocate new signature pins after every preserved ID has been registered.
                    plan.pins_.emplace_back(const_cast<Pin *>(pin), PinId{});
                    continue;
                }
                if (restoring)
                {
                    auto result =
                        plan.topology_.insertPin({pin_id, id, direction, fanCap(pin->kind()), pinSemantic(*pin)});
                    if (!result)
                    {
                        return lux::cxx::unexpected(result.error());
                    }
                }
                else
                {
                    auto result = plan.topology_.addPin(id, direction, fanCap(pin->kind()), pinSemantic(*pin));
                    if (!result)
                    {
                        return lux::cxx::unexpected(result.error());
                    }
                    pin_id = *result;
                }
                plan.pins_.emplace_back(const_cast<Pin *>(pin), pin_id);
            }
        }
        const auto index = plan.nodes_.emplace(std::unique_ptr<Node>{}, 0U);
        plan.nodes_.at(index).index = index;
        plan.insert_.push_back({source, index, id});
        plan.inserted_ids_.push_back(id);
    }
    for (auto &[pin, assigned] : plan.pins_)
    {
        if (!assigned.valid())
        {
            const auto insertion = std::ranges::find_if(
                plan.insert_, [&](const auto &entry) { return entry.source->get() == pin->node(); });
            const auto direction =
                isInput(pin->kind()) ? lux::graph::EPinDirection::INPUT : lux::graph::EPinDirection::OUTPUT;
            auto result = plan.topology_.addPin(insertion->id, direction, fanCap(pin->kind()), pinSemantic(*pin));
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            assigned = *result;
        }
    }
    const auto pin_at = [&](PinId id) -> const Pin * {
        for (const auto &[pin, assigned] : plan.pins_)
        {
            if (assigned == id)
            {
                return pin;
            }
        }
        return graph.findPin(id);
    };
    for (const auto &link : change.connect)
    {
        const auto *from = pin_at(link.from);
        const auto *to = pin_at(link.to);
        if (!from || !to || !final_topology.findPin(link.from) || !final_topology.findPin(link.to))
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
            const auto *output = static_cast<const DataOutPin *>(from)->info().type;
            const auto *input = static_cast<const DataInPin *>(to)->info().type;
            if (!input || !output || !lux::meta::canInitialize(input, output))
            {
                return failure(E::INVALID_TYPE, {}, link.to);
            }
        }
        auto result = plan.topology_.connect(link.from, link.to);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
    }
    for (const auto &entry : change.place)
    {
        auto result = plan.place(entry.node, entry.layout);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
    }
    for (const auto id : change.unplace)
    {
        if (!final_topology.findNode(id))
        {
            return failure(E::UNKNOWN_NODE, id);
        }
        static_cast<void>(plan.layout_.erase(id));
    }
    return plan;
}

std::span<const NodeId> FlowGraphEdit::insertedIds() const noexcept
{
    return inserted_ids_;
}
std::span<const std::pair<Pin *, PinId>> FlowGraphEdit::assignedPins() const noexcept
{
    return pins_;
}

lux::cxx::expected<void, lux::graph::GraphTopologyFailure> FlowGraphEdit::place(NodeId id,
                                                                                lux::graph::GraphNodeLayout value)
{
    if (!target_)
    {
        return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{lux::graph::EGraphTopologyError::INVALID_ID, id});
    }
    const auto &topology = topology_changed_ ? topology_ : target_->topology_;
    if (!topology.findNode(id) || !std::isfinite(value.x) || !std::isfinite(value.y))
    {
        return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{lux::graph::EGraphTopologyError::INVALID_ID, id});
    }
    if (!layout_changed_)
    {
        layout_ = target_->layout_;
        layout_changed_ = true;
    }
    return layout_.set(id, value);
}

void FlowGraphEdit::commit() noexcept
{
    if (!target_)
    {
        std::terminate();
    }
    auto &graph = *target_;
    for (std::size_t i = 0; i < erase_.size(); ++i)
    {
        auto &node = graph.nodes_.at(erase_[i]).node;
        node->assignGraph(nullptr);
        removed_[i] = std::move(node);
    }
    for (const auto index : keep_)
    {
        nodes_.at(index).node = std::move(graph.nodes_.at(index).node);
    }
    for (const auto &[pin, id] : pins_)
    {
        pin->setId(id);
    }
    for (const auto &insertion : insert_)
    {
        auto &node = *insertion.source;
        node->assignStableId(insertion.id);
        node->assignGraph(&graph);
        nodes_.at(insertion.index).node = std::move(node);
    }
    if (storage_changed_)
    {
        std::swap(graph.nodes_, nodes_);
    }
    if (topology_changed_)
    {
        std::swap(graph.topology_, topology_);
    }
    if (layout_changed_)
    {
        std::swap(graph.layout_, layout_);
    }
    target_ = nullptr;
}

std::vector<std::unique_ptr<Node>> FlowGraphEdit::takeRemoved() noexcept
{
    if (target_)
    {
        std::terminate();
    }
    return std::move(removed_);
}
} // namespace lux::flowforge
