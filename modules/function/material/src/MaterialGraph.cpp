#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <utility>
#include <algorithm>
#include <cmath>

namespace lux::material
{
    namespace
    {
        [[nodiscard]] lux::graph::NodeTypeId nodeType(EMatNodeKind kind) noexcept
        {
            return lux::graph::NodeTypeId{static_cast<std::uint64_t>(kind) + 1U};
        }

        [[nodiscard]] lux::graph::PinSemanticId pinSemantic(
            EPinDirection direction,
            std::size_t ordinal
        ) noexcept
        {
            const auto direction_bit = direction == EPinDirection::OUTPUT ? (std::uint64_t{1U} << 63U) : 0U;
            return lux::graph::PinSemanticId{direction_bit | static_cast<std::uint64_t>(ordinal + 1U)};
        }

        [[nodiscard]] lux::graph::EPinDirection graphDirection(EPinDirection direction) noexcept
        {
            return direction == EPinDirection::OUTPUT ? lux::graph::EPinDirection::OUTPUT :
                                                       lux::graph::EPinDirection::INPUT;
        }

        [[nodiscard]] bool isConvertible(EValueType source, EValueType target) noexcept
        {
            if (source == target)
                return true;
            const auto source_width = static_cast<std::uint8_t>(source) + 1U;
            const auto target_width = static_cast<std::uint8_t>(target) + 1U;
            return source_width > target_width || (source == EValueType::FLOAT && target_width > 1U);
        }
    } // namespace

    MaterialGraph::MaterialGraph() = default;
    MaterialGraph::~MaterialGraph() = default;
    MaterialGraph::MaterialGraph(MaterialGraph&&) noexcept = default;
    MaterialGraph& MaterialGraph::operator=(MaterialGraph&&) noexcept = default;

    MaterialGraph MaterialGraph::clone() const
    {
        MaterialGraph result;
        result.shading_model = shading_model;
        result.texture_slots = texture_slots;
        result.param_slots = param_slots;
        result.render_state = render_state;
        result.topology_ = topology_;
        result.layout_ = layout_;
        for (const auto& [id, node_value] : nodes_)
            if (node_value)
                result.nodes_.emplace(id, node_value->clone());
        return result;
    }

    bool MaterialGraph::registerNodeStructure(Node& node_value, bool preserve_pin_ids) noexcept
    {
        const auto add_pins = [&](std::vector<DataPin>& pins, EPinDirection direction, bool existing_pass) noexcept {
            for (std::size_t ordinal{}; ordinal < pins.size(); ++ordinal)
            {
                auto& pin = pins[ordinal];
                pin.direction = direction;
                const bool existing = preserve_pin_ids && pin.id.valid();
                if (existing != existing_pass)
                {
                    continue;
                }
                const auto fan_cap = direction == EPinDirection::INPUT ? 1U : lux::graph::kUnlimitedFan;
                if (existing)
                {
                    const auto inserted = topology_.insertPin(lux::graph::PinRecord{
                        pin.id,
                        node_value.id(),
                        graphDirection(direction),
                        static_cast<std::uint8_t>(fan_cap),
                        pinSemantic(direction, ordinal)
                    });
                    if (!inserted)
                        return false;
                }
                else
                {
                    auto created = topology_.addPin(
                        node_value.id(),
                        graphDirection(direction),
                        static_cast<std::uint8_t>(fan_cap),
                        pinSemantic(direction, ordinal)
                    );
                    if (!created)
                        return false;
                    pin.id = *created;
                }
            }
            return true;
        };

        // Admit all preserved identities before issuing new pins, including outputs after new inputs.
        if (!add_pins(node_value.inputs(), EPinDirection::INPUT, true) ||
            !add_pins(node_value.outputs(), EPinDirection::OUTPUT, true) ||
            !add_pins(node_value.inputs(), EPinDirection::INPUT, false) ||
            !add_pins(node_value.outputs(), EPinDirection::OUTPUT, false))
        {
            static_cast<void>(topology_.detachNode(node_value.id()));
            return false;
        }
        return true;
    }

    NodeId MaterialGraph::addNode(std::unique_ptr<Node> node_value)
    {
        if (!node_value)
            return {};
        auto id = topology_.addNode(nodeType(node_value->kind()));
        if (!id)
            return {};
        node_value->setId(*id);
        if (!registerNodeStructure(*node_value, false))
            return {};
        try
        {
            nodes_.emplace(*id, std::move(node_value));
            return *id;
        }
        catch (...)
        {
            static_cast<void>(topology_.detachNode(*id));
            return {};
        }
    }

    NodeId MaterialGraph::addNodeWithId(NodeId id, std::unique_ptr<Node> node_value)
    {
        if (!node_value || !id.valid() || nodes_.find(id) != nodes_.end())
            return {};
        if (!topology_.insertNode(lux::graph::NodeRecord{id, nodeType(node_value->kind())}))
            return {};
        node_value->setId(id);
        const auto pins_have_ids = [&] {
            for (const auto& pin : node_value->inputs())
                if (!pin.id.valid())
                    return false;
            for (const auto& pin : node_value->outputs())
                if (!pin.id.valid())
                    return false;
            return true;
        }();
        if (!registerNodeStructure(*node_value, pins_have_ids))
            return {};
        try
        {
            nodes_.emplace(id, std::move(node_value));
            return id;
        }
        catch (...)
        {
            static_cast<void>(topology_.detachNode(id));
            return {};
        }
    }

    std::unique_ptr<Node> MaterialGraph::extractNode(NodeId id)
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            return nullptr;
        if (!topology_.detachNode(id))
            return nullptr;
        auto result = std::move(found->second);
        nodes_.erase(found);
        return result;
    }

    Node* MaterialGraph::node(NodeId id) noexcept
    {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : found->second.get();
    }

    const Node* MaterialGraph::node(NodeId id) const noexcept
    {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : found->second.get();
    }

    void MaterialGraph::removeNode(NodeId id)
    {
        if (!topology_.detachNode(id))
            return;
        nodes_.erase(id);
        static_cast<void>(layout_.erase(id));
    }

    bool MaterialGraph::canConnect(NodeId src, uint32_t src_pin, NodeId dst, uint32_t dst_pin) const noexcept
    {
        const auto* source_node = node(src);
        const auto* target_node = node(dst);
        const bool has_nodes = source_node != nullptr && target_node != nullptr;
        const bool has_valid_pins = has_nodes && src_pin < source_node->outputs().size() &&
            dst_pin < target_node->inputs().size();
        const bool is_invalid_connection = src == dst || !has_valid_pins;
        if (is_invalid_connection)
        {
            return false;
        }
        return isConvertible(source_node->outputs()[src_pin].type, target_node->inputs()[dst_pin].type);
    }

    bool MaterialGraph::connect(NodeId src, uint32_t src_pin, NodeId dst, uint32_t dst_pin)
    {
        if (!canConnect(src, src_pin, dst, dst_pin))
            return false;
        auto* source_node = node(src);
        auto* target_node = node(dst);
        return static_cast<bool>(topology_.connect(
            source_node->outputs()[src_pin].id,
            target_node->inputs()[dst_pin].id
        ));
    }

    void MaterialGraph::disconnect(NodeId dst, uint32_t dst_pin)
    {
        auto* target_node = node(dst);
        if (target_node == nullptr || dst_pin >= target_node->inputs().size())
            return;
        const auto incoming = topology_.incoming(target_node->inputs()[dst_pin].id);
        if (incoming)
            static_cast<void>(topology_.disconnect(incoming->from, incoming->to));
    }

    PinLink MaterialGraph::source(NodeId dst, uint32_t dst_pin) const noexcept
    {
        const auto* target_node = node(dst);
        if (target_node == nullptr || dst_pin >= target_node->inputs().size())
            return {};
        return source(target_node->inputs()[dst_pin].id);
    }

    PinLink MaterialGraph::source(PinId input) const noexcept
    {
        const auto incoming = topology_.incoming(input);
        if (!incoming)
            return {};
        const auto* source_pin = topology_.findPin(incoming->from);
        if (source_pin == nullptr)
            return {};
        const auto* source_node = node(source_pin->owner);
        if (source_node == nullptr)
            return {};
        for (std::uint32_t ordinal{}; ordinal < source_node->outputs().size(); ++ordinal)
            if (source_node->outputs()[ordinal].id == incoming->from)
                return PinLink{source_pin->owner, ordinal};
        return {};
    }

    MaterialGraphEdit::MaterialGraphEdit(MaterialGraph& source) : target_(&source) {}
    MaterialGraphEdit::~MaterialGraphEdit() = default;
    MaterialGraphEdit::MaterialGraphEdit(MaterialGraphEdit&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)), topology_(std::move(other.topology_)),
          layout_(std::move(other.layout_)), nodes_(std::move(other.nodes_)), inserted_(std::move(other.inserted_)),
          committed_(std::exchange(other.committed_, true)), topology_changed_(other.topology_changed_)
    {
    }

    lux::cxx::expected<MaterialGraphEdit, lux::graph::GraphTopologyFailure> MaterialGraphEdit::prepare(
        MaterialGraph& source, const MaterialGraphChange& change)
    {
        using Error = lux::graph::EGraphTopologyError;
        const auto fail = [](Error error, NodeId node = {}, PinId pin = {})
        {
            return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{error, node, pin});
        };
        const bool changes_topology = !change.insert.empty() || !change.erase.empty() ||
            !change.connect.empty() || !change.disconnect.empty();
        if (!changes_topology)
        {
            MaterialGraphEdit result(source);
            result.layout_ = source.layout_;
            for (const auto& entry : change.place)
            {
                const auto placed = result.place(entry.node, entry.layout);
                if (!placed)
                {
                    return lux::cxx::unexpected(placed.error());
                }
            }
            for (const auto id : change.unplace)
            {
                static_cast<void>(result.layout_.erase(id));
            }
            return result;
        }
        // Only structural records are staged. Existing node payloads are never cloned.
        MaterialGraph stage;
        stage.topology_ = source.topology_;
        stage.layout_ = source.layout_;
        MaterialGraphEdit result(source);
        result.topology_changed_ = true;
        result.nodes_.reserve(change.insert.size() + change.erase.size());
        result.inserted_.reserve(change.insert.size());
        for (const auto& link : change.disconnect)
        {
            auto disconnected = stage.topology_.disconnect(link.from, link.to);
            if (!disconnected)
            {
                return lux::cxx::unexpected(disconnected.error());
            }
        }
        for (const auto id : change.erase)
        {
            if (!source.node(id))
            {
                return fail(Error::UNKNOWN_NODE, id);
            }
            auto removed = stage.topology_.detachNode(id);
            if (!removed)
            {
                return lux::cxx::unexpected(removed.error());
            }
            static_cast<void>(stage.layout_.erase(id));
            result.nodes_.emplace_back(id, NodeStorage::node_type{});
        }
        for (const auto* node : change.insert)
        {
            if (!node)
            {
                return fail(Error::INVALID_TYPE);
            }
            if (node->id().valid() && stage.topology_.findNode(node->id()))
            {
                return fail(Error::DUPLICATE_NODE, node->id());
            }
            auto copy = node->clone();
            const auto id = node->id().valid() ? stage.addNodeWithId(node->id(), std::move(copy)) : stage.addNode(std::move(copy));
            if (!id.valid())
            {
                return fail(Error::INVALID_ID, node->id());
            }
            result.inserted_.push_back(stage.node(id));
        }
        const auto findNode = [&](NodeId id) -> const Node*
        {
            if (const auto* inserted = stage.node(id))
            {
                return inserted;
            }
            return source.node(id);
        };
        for (const auto& link : change.connect)
        {
            const auto* from = stage.topology_.findPin(link.from);
            const auto* to = stage.topology_.findPin(link.to);
            if (!from || !to)
            {
                return fail(Error::UNKNOWN_PIN, {}, from ? link.to : link.from);
            }
            if (from->direction != lux::graph::EPinDirection::OUTPUT || to->direction != lux::graph::EPinDirection::INPUT)
            {
                return fail(Error::DIRECTION_MISMATCH, {}, link.from);
            }
            const auto* from_node = findNode(from->owner);
            const auto* to_node = findNode(to->owner);
            if (!from_node || !to_node)
            {
                return fail(Error::UNKNOWN_NODE, from_node ? to->owner : from->owner);
            }
            const auto& outputs = from_node->outputs();
            const auto& inputs = to_node->inputs();
            const auto output = std::ranges::find(outputs, link.from, &DataPin::id);
            const auto input = std::ranges::find(inputs, link.to, &DataPin::id);
            if (output == outputs.end() || input == inputs.end() || !isConvertible(output->type, input->type))
            {
                return fail(Error::INVALID_TYPE, from->owner, link.from);
            }
            auto connected = stage.topology_.connect(link.from, link.to);
            if (!connected)
            {
                return lux::cxx::unexpected(connected.error());
            }
        }
        for (const auto& entry : change.place)
        {
            if (!stage.topology_.findNode(entry.node))
            {
                return fail(Error::UNKNOWN_NODE, entry.node);
            }
            if (!std::isfinite(entry.layout.x) || !std::isfinite(entry.layout.y))
            {
                return fail(Error::INVALID_SEMANTIC, entry.node);
            }
            auto placed = stage.layout_.set(entry.node, entry.layout);
            if (!placed)
            {
                return lux::cxx::unexpected(placed.error());
            }
        }
        for (const auto id : change.unplace)
        {
            static_cast<void>(stage.layout_.erase(id));
        }
        for (const auto* node : result.inserted_)
        {
            result.nodes_.emplace_back(node->id(), stage.nodes_.extract(node->id()));
        }
        result.topology_ = std::move(stage.topology_);
        result.layout_ = std::move(stage.layout_);
        // reserve may change capacity, never content or identity. Insertion later transfers node handles.
        source.nodes_.reserve(source.nodes_.size() + change.insert.size());
        return result;
    }

    std::span<const Node* const> MaterialGraphEdit::insertedNodes() const noexcept { return inserted_; }

    lux::cxx::expected<void, lux::graph::GraphTopologyFailure> MaterialGraphEdit::place(
        NodeId id, lux::graph::GraphNodeLayout value)
    {
        if (committed_ || !target_ || !std::isfinite(value.x) || !std::isfinite(value.y))
        {
            return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{lux::graph::EGraphTopologyError::INVALID_ID, id});
        }
        const auto& topology = topology_changed_ ? topology_ : target_->topology_;
        if (!topology.findNode(id))
        {
            return lux::cxx::unexpected(lux::graph::GraphTopologyFailure{lux::graph::EGraphTopologyError::UNKNOWN_NODE, id});
        }
        return layout_.set(id, value);
    }

    void MaterialGraphEdit::commit() noexcept
    {
        if (committed_ || !target_)
        {
            std::terminate();
        }
        using std::swap;
        if (topology_changed_)
        {
            swap(target_->topology_, topology_);
        }
        swap(target_->layout_, layout_);
        for (auto& [id, node] : nodes_)
        {
            if (node.empty())
            {
                node = target_->nodes_.extract(id);
            }
            else
            {
                const auto inserted = target_->nodes_.insert(std::move(node));
                if (!inserted.inserted)
                {
                    std::terminate();
                }
            }
        }
        committed_ = true;
    }

} // namespace lux::material
