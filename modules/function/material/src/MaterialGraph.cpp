#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <utility>

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
        const auto add_pins = [&](std::vector<DataPin>& pins, EPinDirection direction) noexcept {
            for (std::size_t ordinal{}; ordinal < pins.size(); ++ordinal)
            {
                auto& pin = pins[ordinal];
                pin.direction = direction;
                const auto fan_cap = direction == EPinDirection::INPUT ? 1U : lux::graph::kUnlimitedFan;
                if (preserve_pin_ids && pin.id.valid())
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

        if (!add_pins(node_value.inputs(), EPinDirection::INPUT) ||
            !add_pins(node_value.outputs(), EPinDirection::OUTPUT))
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
} // namespace lux::material
