#include <lux/engine/editor/material/MaterialGraphEditorAdapter.hpp>

#include <lux/engine/material/graph/Nodes.hpp>

#include <memory>
#include <new>
#include <utility>

namespace lux::editor::material_graph
{
    namespace
    {
        struct Capture final
        {
            std::unique_ptr<material::Node> node;
            graph::GraphNodeLayout layout;
            bool has_layout{};
        };

        [[nodiscard]] graph::NodeTypeId typeId(material::EMatNodeKind kind) noexcept
        {
            return graph::NodeTypeId{static_cast<std::uint64_t>(kind) + 1U};
        }

        [[nodiscard]] material::EMatNodeKind nodeKind(graph::NodeTypeId type) noexcept
        {
            if (!type.valid() || type.value <= 1U ||
                type.value > static_cast<std::uint64_t>(material::EMatNodeKind::COUNT))
            {
                return material::EMatNodeKind::INVALID;
            }
            return static_cast<material::EMatNodeKind>(type.value - 1U);
        }

        [[nodiscard]] std::unique_ptr<material::Node> makeNode(material::EMatNodeKind kind)
        {
            switch (kind)
            {
            case material::EMatNodeKind::CONSTANT: return std::make_unique<material::ConstantNode>();
            case material::EMatNodeKind::INPUT: return std::make_unique<material::InputNode>();
            case material::EMatNodeKind::SAMPLE_TEXTURE: return std::make_unique<material::SampleTextureNode>();
            case material::EMatNodeKind::MATH: return std::make_unique<material::MathNode>();
            case material::EMatNodeKind::SWIZZLE: return std::make_unique<material::SwizzleNode>();
            case material::EMatNodeKind::CONSTRUCT: return std::make_unique<material::ConstructNode>();
            case material::EMatNodeKind::DECODE_NORMAL: return std::make_unique<material::DecodeNormalNode>();
            case material::EMatNodeKind::TBN_TRANSFORM: return std::make_unique<material::TbnTransformNode>();
            case material::EMatNodeKind::PARAM: return std::make_unique<material::ParamNode>();
            case material::EMatNodeKind::OUTPUT_SURFACE: return std::make_unique<material::OutputSurfaceNode>();
            default: return nullptr;
            }
        }

        [[nodiscard]] const material::DataPin* findPin(
            const material::MaterialGraph& source,
            graph::PinId id
        ) noexcept
        {
            for (const auto& [node_id, node] : source.nodes())
            {
                static_cast<void>(node_id);
                for (const auto& pin : node->inputs())
                    if (pin.id == id)
                        return &pin;
                for (const auto& pin : node->outputs())
                    if (pin.id == id)
                        return &pin;
            }
            return nullptr;
        }

        [[nodiscard]] std::optional<std::uint32_t> pinOrdinal(
            const material::Node& node,
            graph::PinId id,
            material::EPinDirection direction
        ) noexcept
        {
            const auto& pins = direction == material::EPinDirection::INPUT ? node.inputs() : node.outputs();
            for (std::uint32_t ordinal{}; ordinal < pins.size(); ++ordinal)
                if (pins[ordinal].id == id)
                    return ordinal;
            return std::nullopt;
        }
    } // namespace

    MaterialGraphDocument::MaterialGraphDocument(material::MaterialGraph& graph) noexcept : graph_(&graph) {}
    graph::GraphTopology& MaterialGraphDocument::topology() noexcept { return graph_->topology(); }
    const graph::GraphTopology& MaterialGraphDocument::topology() const noexcept { return graph_->topology(); }
    graph::GraphLayout& MaterialGraphDocument::layout() noexcept { return graph_->layout(); }
    const graph::GraphLayout& MaterialGraphDocument::layout() const noexcept { return graph_->layout(); }

    graph::NodeId MaterialGraphDocument::addNode(graph::NodeTypeId type)
    {
        try
        {
            auto node = makeNode(nodeKind(type));
            return node ? graph_->addNode(std::move(node)) : graph::NodeId{};
        }
        catch (const std::bad_alloc&)
        {
            return {};
        }
    }

    node_graph::NodeCapture MaterialGraphDocument::detachNode(graph::NodeId node)
    {
        std::shared_ptr<Capture> capture;
        try
        {
            capture = std::make_shared<Capture>();
        }
        catch (const std::bad_alloc&)
        {
            return {};
        }
        const auto* layout_value = graph_->layout().find(node);
        auto detached = graph_->extractNode(node);
        if (!detached)
            return {};
        capture->node = std::move(detached);
        if (layout_value != nullptr)
        {
            capture->layout = *layout_value;
            capture->has_layout = true;
        }
        static_cast<void>(graph_->layout().erase(node));
        return capture;
    }

    bool MaterialGraphDocument::attachNode(graph::NodeId original, node_graph::NodeCapture capture)
    {
        auto typed = std::static_pointer_cast<Capture>(std::move(capture));
        if (!typed || !typed->node || graph_->node(original) != nullptr)
            return false;
        auto clone = typed->node->clone();
        if (graph_->addNodeWithId(original, std::move(clone)) != original)
            return false;
        if (typed->has_layout && !graph_->layout().set(original, typed->layout))
        {
            graph_->removeNode(original);
            return false;
        }
        return true;
    }

    std::optional<node_graph::NodeActionJournal>
    MaterialGraphDocument::invokeNodeAction(graph::NodeId, std::uint64_t)
    {
        return std::nullopt;
    }

    bool MaterialGraphDocument::restoreNodeAction(graph::NodeId, node_graph::NodeCapture)
    {
        return false;
    }

    material::MaterialGraph& MaterialGraphDocument::source() noexcept { return *graph_; }
    const material::MaterialGraph& MaterialGraphDocument::source() const noexcept { return *graph_; }

    MaterialGraphRules::MaterialGraphRules(const material::MaterialGraph& graph) noexcept : graph_(&graph) {}

    bool MaterialGraphRules::canConnect(
        const node_graph::IGraphDocument&,
        graph::PinId from,
        graph::PinId to
    ) const noexcept
    {
        const auto* from_record = graph_->topology().findPin(from);
        const auto* to_record = graph_->topology().findPin(to);
        const auto* from_node = from_record ? graph_->node(from_record->owner) : nullptr;
        const auto* to_node = to_record ? graph_->node(to_record->owner) : nullptr;
        if (from_node == nullptr || to_node == nullptr)
            return false;
        const auto from_ordinal = pinOrdinal(*from_node, from, material::EPinDirection::OUTPUT);
        const auto to_ordinal = pinOrdinal(*to_node, to, material::EPinDirection::INPUT);
        return from_ordinal && to_ordinal &&
            graph_->canConnect(from_node->id(), *from_ordinal, to_node->id(), *to_ordinal);
    }

    MaterialGraphPresentation::MaterialGraphPresentation(const material::MaterialGraph& graph) : graph_(&graph)
    {
        for (std::uint64_t value = static_cast<std::uint64_t>(material::EMatNodeKind::CONSTANT);
             value < static_cast<std::uint64_t>(material::EMatNodeKind::COUNT);
             ++value)
        {
            const auto kind = static_cast<material::EMatNodeKind>(value);
            palette_.push_back({typeId(kind), material::toString(kind), "Material"});
        }
    }

    node_graph::GraphNodePresentation MaterialGraphPresentation::node(graph::NodeId id) const noexcept
    {
        const auto* value = graph_->node(id);
        return {value ? std::string_view{value->name()} : std::string_view{"Unknown Material Node"}};
    }

    node_graph::GraphPinPresentation MaterialGraphPresentation::pin(graph::PinId id) const noexcept
    {
        const auto* value = findPin(*graph_, id);
        return {value ? std::string_view{value->name} : std::string_view{"Unknown Pin"}};
    }

    std::span<const node_graph::GraphPaletteEntry> MaterialGraphPresentation::palette() const noexcept
    {
        return palette_;
    }
} // namespace lux::editor::material_graph
