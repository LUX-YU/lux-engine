#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <algorithm>
#include <limits>
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

        [[nodiscard]] lux::graph::PinSemanticId pinSemantic(const Pin& pin) noexcept
        {
            const auto kind = static_cast<std::uint64_t>(pin.kind()) + 1U;
            const auto& pins = isInput(pin.kind()) ? pin.node()->inPins() : pin.node()->outPins();
            const auto found = std::ranges::find(pins, std::addressof(pin));
            const auto ordinal = found == pins.end() ? 0U : static_cast<std::uint64_t>(found - pins.begin()) + 1U;
            return lux::graph::PinSemanticId{(kind << 56U) | ordinal};
        }
    } // namespace

    FlowGraph::FlowGraph() = default;

    FlowGraph::~FlowGraph()
    {
        for (auto& storage : nodes_.values())
            if (storage.node)
                storage.node->assignGraph(nullptr);
    }

    FlowGraph::FlowGraph(FlowGraph&& other) noexcept
        : variables_(std::move(other.variables_)),
          exports_(std::move(other.exports_)),
          next_var_id_(other.next_var_id_),
          nodes_(std::move(other.nodes_)),
          topology_(std::move(other.topology_)),
          layout_(std::move(other.layout_))
    {
        rebindNodes();
    }

    FlowGraph& FlowGraph::operator=(FlowGraph&& other) noexcept
    {
        if (this == &other)
            return *this;
        for (auto& storage : nodes_.values())
            if (storage.node)
                storage.node->assignGraph(nullptr);
        variables_ = std::move(other.variables_);
        exports_ = std::move(other.exports_);
        next_var_id_ = other.next_var_id_;
        nodes_ = std::move(other.nodes_);
        topology_ = std::move(other.topology_);
        layout_ = std::move(other.layout_);
        rebindNodes();
        return *this;
    }

    const std::vector<NodeStorage>& FlowGraph::nodes() const
    {
        return nodes_.values();
    }

    size_t FlowGraph::addNodes(std::unique_ptr<Node> node)
    {
        if (!node)
            return (std::numeric_limits<std::size_t>::max)();
        auto id = topology_.addNode(nodeType(node->operation()));
        if (!id)
            return (std::numeric_limits<std::size_t>::max)();
        node->assignStableId(*id);
        if (!attachNodeStructure(*node, false))
            return (std::numeric_limits<std::size_t>::max)();
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
            return (std::numeric_limits<std::size_t>::max)();
        if (!topology_.insertNode(lux::graph::NodeRecord{stable_id, nodeType(node->operation())}))
            return (std::numeric_limits<std::size_t>::max)();
        node->assignStableId(stable_id);
        const auto preserve_pin_ids = std::ranges::all_of(node->inPins(), [](const Pin* pin) {
            return pin != nullptr && pin->id().valid();
        }) && std::ranges::all_of(node->outPins(), [](const Pin* pin) {
            return pin != nullptr && pin->id().valid();
        });
        if (!attachNodeStructure(*node, preserve_pin_ids))
            return (std::numeric_limits<std::size_t>::max)();
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

    Node* FlowGraph::findNodeById(NodeId stable_id)
    {
        for (auto& storage : nodes_.values())
            if (storage.node && storage.node->id() == stable_id)
                return storage.node.get();
        return nullptr;
    }

    const Node* FlowGraph::findNodeById(NodeId stable_id) const
    {
        for (const auto& storage : nodes_.values())
            if (storage.node && storage.node->id() == stable_id)
                return storage.node.get();
        return nullptr;
    }

    bool FlowGraph::removeNode(size_t index)
    {
        if (!nodes_.contains(index))
            return false;
        auto& node = *nodes_.at(index).node;
        if (!topology_.detachNode(node.id()))
            return false;
        static_cast<void>(layout_.erase(node.id()));
        node.assignGraph(nullptr);
        nodes_.erase(index);
        return true;
    }

    bool FlowGraph::extractNode(size_t index, NodeStorage& out)
    {
        if (!nodes_.contains(index))
            return false;
        auto& node = *nodes_.at(index).node;
        if (!topology_.detachNode(node.id()))
            return false;
        node.assignGraph(nullptr);
        return nodes_.extract(index, out);
    }

    bool FlowGraph::insertNodeAt(size_t index, std::unique_ptr<Node> node)
    {
        if (!node || !node->id().valid() || nodes_.contains(index))
            return false;
        const auto id = node->id();
        if (!topology_.insertNode(lux::graph::NodeRecord{id, nodeType(node->operation())}))
            return false;
        if (!attachNodeStructure(*node, true))
            return false;
        if (!nodes_.try_emplace_at(index, std::move(node), 0U))
        {
            static_cast<void>(topology_.detachNode(id));
            return false;
        }
        nodes_.at(index).index = index;
        return true;
    }

    Pin* FlowGraph::findPin(PinId id) noexcept
    {
        for (auto& storage : nodes_.values())
        {
            if (!storage.node)
                continue;
            for (auto* pin : storage.node->inPins())
                if (pin != nullptr && pin->id() == id)
                    return pin;
            for (auto* pin : storage.node->outPins())
                if (pin != nullptr && pin->id() == id)
                    return pin;
        }
        return nullptr;
    }

    const Pin* FlowGraph::findPin(PinId id) const noexcept
    {
        return const_cast<FlowGraph*>(this)->findPin(id);
    }

    std::vector<Pin*> FlowGraph::linkedPins(PinId id)
    {
        std::vector<Pin*> result;
        for (const auto& link : topology_.links())
        {
            if (link.from == id)
                result.push_back(findPin(link.to));
            else if (link.to == id)
                result.push_back(findPin(link.from));
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
                result.push_back(findPin(link.to));
            else if (link.to == id)
                result.push_back(findPin(link.from));
        }
        std::erase(result, nullptr);
        return result;
    }

    ELinkError FlowGraph::connect(Pin& first, Pin& second) noexcept
    {
        const auto first_preflight = first.canLink(std::addressof(second));
        if (first_preflight != ELinkError::SUCCESS)
            return first_preflight;
        const auto second_preflight = second.canLink(std::addressof(first));
        if (second_preflight != ELinkError::SUCCESS)
            return second_preflight;

        auto* first_record = topology_.findPin(first.id());
        auto* second_record = topology_.findPin(second.id());
        if (first_record == nullptr || second_record == nullptr)
            return ELinkError::INVALID_PIN;
        const auto from = first_record->direction == lux::graph::EPinDirection::OUTPUT ? first.id() : second.id();
        const auto to = first_record->direction == lux::graph::EPinDirection::INPUT ? first.id() : second.id();
        const auto connected = topology_.connect(from, to);
        if (connected)
            return ELinkError::SUCCESS;
        if (connected.error().code == lux::graph::EGraphTopologyError::DUPLICATE_LINK ||
            connected.error().code == lux::graph::EGraphTopologyError::FAN_CAP_EXCEEDED)
        {
            return ELinkError::HAS_LINKED;
        }
        return ELinkError::WRONG_KIND;
    }

    ELinkError FlowGraph::disconnect(Pin& first, Pin& second) noexcept
    {
        const auto* first_record = topology_.findPin(first.id());
        const auto* second_record = topology_.findPin(second.id());
        if (first_record == nullptr || second_record == nullptr)
            return ELinkError::INVALID_PIN;
        const auto from = first_record->direction == lux::graph::EPinDirection::OUTPUT ? first.id() : second.id();
        const auto to = first_record->direction == lux::graph::EPinDirection::INPUT ? first.id() : second.id();
        return topology_.disconnect(from, to) ? ELinkError::UNLINKED : ELinkError::UNMATCHED;
    }

    bool FlowGraph::registerPin(Pin& pin) noexcept
    {
        const auto direction = isInput(pin.kind()) ? lux::graph::EPinDirection::INPUT :
                                                    lux::graph::EPinDirection::OUTPUT;
        if (const auto* existing = topology_.findPin(pin.id()); existing != nullptr)
        {
            return existing->owner == pin.node()->id() && existing->direction == direction &&
                existing->semantic == pinSemantic(pin);
        }
        if (pin.id().valid())
        {
            return static_cast<bool>(topology_.insertPin(lux::graph::PinRecord{
                pin.id(),
                pin.node()->id(),
                direction,
                fanCap(pin.kind()),
                pinSemantic(pin)
            }));
        }
        auto created = topology_.addPin(
            pin.node()->id(),
            direction,
            fanCap(pin.kind()),
            pinSemantic(pin)
        );
        if (!created)
            return false;
        pin.setId(*created);
        return true;
    }

    void FlowGraph::unregisterPin(Pin& pin) noexcept
    {
        if (pin.id().valid())
            static_cast<void>(topology_.detachPin(pin.id()));
    }

    bool FlowGraph::assignPinId(Pin& pin, PinId id) noexcept
    {
        if (!id.valid() || topology_.findPin(id) != nullptr)
            return false;
        unregisterPin(pin);
        pin.setId(id);
        if (registerPin(pin))
            return true;
        pin.setId({});
        return false;
    }

    bool FlowGraph::attachNodeStructure(Node& node, bool preserve_pin_ids) noexcept
    {
        if (!preserve_pin_ids)
        {
            for (auto* pin : node.inPins())
                if (pin != nullptr)
                    pin->setId({});
            for (auto* pin : node.outPins())
                if (pin != nullptr)
                    pin->setId({});
        }
        node.assignGraph(this);
        for (auto* pin : node.inPins())
            if (pin == nullptr || !registerPin(*pin))
            {
                node.assignGraph(nullptr);
                static_cast<void>(topology_.detachNode(node.id()));
                return false;
            }
        for (auto* pin : node.outPins())
            if (pin == nullptr || !registerPin(*pin))
            {
                node.assignGraph(nullptr);
                static_cast<void>(topology_.detachNode(node.id()));
                return false;
            }
        return true;
    }

    void FlowGraph::rebindNodes() noexcept
    {
        for (auto& storage : nodes_.values())
            if (storage.node)
                storage.node->assignGraph(this);
    }
} // namespace lux::flowforge
