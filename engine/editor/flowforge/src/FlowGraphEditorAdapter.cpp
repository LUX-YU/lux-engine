#include <lux/engine/editor/flowforge/FlowGraphEditorAdapter.hpp>

#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/meta/MetaCompat.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace lux::editor::flow_graph
{
    namespace
    {
        struct Capture final
        {
            std::size_t index{(std::numeric_limits<std::size_t>::max)()};
            flowforge::NodeStorage storage;
            graph::GraphNodeLayout layout;
            bool has_layout{};
        };

        struct SequenceActionState final
        {
            std::vector<graph::PinId> outputs;
        };

        [[nodiscard]] graph::NodeTypeId typeId(flowforge::ENodeOperation operation) noexcept
        {
            return graph::NodeTypeId{static_cast<std::uint64_t>(operation) + 1U};
        }

        [[nodiscard]] flowforge::ENodeOperation nodeOperation(graph::NodeTypeId type) noexcept
        {
            if (!type.valid() || type.value <= 1U ||
                type.value > static_cast<std::uint64_t>(flowforge::ENodeOperation::SEND_EVENT) + 1U)
            {
                return flowforge::ENodeOperation::INVALID;
            }
            return static_cast<flowforge::ENodeOperation>(type.value - 1U);
        }

        [[nodiscard]] std::unique_ptr<flowforge::Node> makeNode(flowforge::ENodeOperation operation)
        {
            switch (operation)
            {
            case flowforge::ENodeOperation::START: return std::make_unique<flowforge::StartNode>();
            case flowforge::ENodeOperation::BRANCH: return std::make_unique<flowforge::BranchNode>();
            case flowforge::ENodeOperation::SEQUENCE: return std::make_unique<flowforge::SequenceNode>();
            default: return nullptr;
            }
        }

        [[nodiscard]] const flowforge::NodeStorage* findStorage(
            const flowforge::FlowGraph& source,
            graph::NodeId id
        ) noexcept
        {
            for (const auto& storage : source.nodes())
                if (storage.node && storage.node->id() == id)
                    return &storage;
            return nullptr;
        }
    } // namespace

    FlowGraphDocument::FlowGraphDocument(flowforge::FlowGraph& graph) noexcept : graph_(&graph) {}
    graph::GraphTopology& FlowGraphDocument::topology() noexcept { return graph_->topology(); }
    const graph::GraphTopology& FlowGraphDocument::topology() const noexcept { return graph_->topology(); }
    graph::GraphLayout& FlowGraphDocument::layout() noexcept { return graph_->layout(); }
    const graph::GraphLayout& FlowGraphDocument::layout() const noexcept { return graph_->layout(); }

    graph::NodeId FlowGraphDocument::addNode(graph::NodeTypeId type)
    {
        try
        {
            auto node = makeNode(nodeOperation(type));
            if (!node)
                return {};
            auto* raw = node.get();
            const auto index = graph_->addNodes(std::move(node));
            return index == (std::numeric_limits<std::size_t>::max)() ? graph::NodeId{} : raw->id();
        }
        catch (const std::bad_alloc&)
        {
            return {};
        }
    }

    node_graph::NodeCapture FlowGraphDocument::detachNode(graph::NodeId node)
    {
        const auto* storage = findStorage(*graph_, node);
        if (storage == nullptr)
            return {};
        std::shared_ptr<Capture> capture;
        try
        {
            capture = std::make_shared<Capture>();
        }
        catch (const std::bad_alloc&)
        {
            return {};
        }
        capture->index = storage->index;
        if (const auto* layout_value = graph_->layout().find(node))
        {
            capture->layout = *layout_value;
            capture->has_layout = true;
        }
        if (!graph_->extractNode(storage->index, capture->storage))
            return {};
        static_cast<void>(graph_->layout().erase(node));
        return capture;
    }

    bool FlowGraphDocument::attachNode(graph::NodeId original, node_graph::NodeCapture capture)
    {
        auto typed = std::static_pointer_cast<Capture>(std::move(capture));
        if (!typed || !typed->storage.node || typed->storage.node->id() != original)
            return false;
        if (!graph_->insertNodeAt(typed->index, std::move(typed->storage.node)))
            return false;
        if (typed->has_layout && !graph_->layout().set(original, typed->layout))
        {
            graph_->removeNode(typed->index);
            return false;
        }
        return true;
    }

    std::optional<node_graph::NodeActionJournal>
    FlowGraphDocument::invokeNodeAction(graph::NodeId node, std::uint64_t action)
    {
        auto* value = graph_->findNodeById(node);
        if (value == nullptr || value->operation() != flowforge::ENodeOperation::SEQUENCE)
            return std::nullopt;
        auto& sequence = *static_cast<flowforge::SequenceNode*>(value);
        const auto current = sequence.execOutPins().size();
        const auto requested = static_cast<EFlowGraphNodeAction>(action);
        const bool is_add = requested == EFlowGraphNodeAction::ADD_SEQUENCE_OUTPUT;
        const bool is_remove = requested == EFlowGraphNodeAction::REMOVE_SEQUENCE_OUTPUT;
        if (!is_add && (!is_remove || current == 0U))
            return std::nullopt;
        if (is_remove && graph_->topology().linkCount(sequence.execOutPins().back()->id()) != 0U)
            return std::nullopt;

        try
        {
            auto before = std::make_shared<SequenceActionState>();
            before->outputs.reserve(current);
            for (const auto& pin : sequence.execOutPins())
                before->outputs.push_back(pin->id());
            auto after = std::make_shared<SequenceActionState>(*before);
            if (is_add)
            {
                after->outputs.push_back({});
                auto* added = sequence.addExecOutPin();
                if (added == nullptr)
                    return std::nullopt;
                after->outputs.back() = added->id();
            }
            else
            {
                after->outputs.pop_back();
                static_cast<void>(sequence.removeExecOutPin());
            }
            return node_graph::NodeActionJournal{std::move(before), std::move(after)};
        }
        catch (const std::bad_alloc&)
        {
            return std::nullopt;
        }
    }

    bool FlowGraphDocument::restoreNodeAction(graph::NodeId node, node_graph::NodeCapture state)
    {
        auto* value = graph_->findNodeById(node);
        auto target = std::static_pointer_cast<SequenceActionState>(std::move(state));
        if (value == nullptr || value->operation() != flowforge::ENodeOperation::SEQUENCE || !target)
            return false;
        auto& sequence = *static_cast<flowforge::SequenceNode*>(value);
        const auto current = sequence.execOutPins().size();
        const auto prefix_count = (std::min)(current, target->outputs.size());
        for (std::size_t index{}; index < prefix_count; ++index)
            if (sequence.execOutPins()[index]->id() != target->outputs[index])
                return false;
        try
        {
            if (target->outputs.size() == current)
                return true;
            if (target->outputs.size() == current + 1U)
            {
                auto* added = sequence.addExecOutPin(target->outputs.back());
                return added != nullptr && added->id() == target->outputs.back();
            }
            if (current != 0U && target->outputs.size() + 1U == current)
            {
                const auto* removed = sequence.execOutPins().back().get();
                if (graph_->topology().linkCount(removed->id()) != 0U)
                    return false;
                static_cast<void>(sequence.removeExecOutPin());
                return true;
            }
            return false;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    flowforge::FlowGraph& FlowGraphDocument::source() noexcept { return *graph_; }
    const flowforge::FlowGraph& FlowGraphDocument::source() const noexcept { return *graph_; }

    FlowGraphRules::FlowGraphRules(const flowforge::FlowGraph& graph) noexcept : graph_(&graph) {}

    bool FlowGraphRules::canConnect(
        const node_graph::IGraphDocument&,
        graph::PinId from,
        graph::PinId to
    ) const noexcept
    {
        const auto* output = graph_->findPin(from);
        const auto* input = graph_->findPin(to);
        if (output == nullptr || input == nullptr || output->node() == input->node())
            return false;
        if (output->kind() == flowforge::EPinKind::EXEC_OUT)
            return input->kind() == flowforge::EPinKind::EXEC_IN;
        if (output->kind() != flowforge::EPinKind::DATA_OUT || input->kind() != flowforge::EPinKind::DATA_IN)
            return false;
        const auto& out = *static_cast<const flowforge::DataOutPin*>(output);
        const auto& in = *static_cast<const flowforge::DataInPin*>(input);
        return lux::meta::canInitialize(in.info().type, out.info().type);
    }

    FlowGraphPresentation::FlowGraphPresentation(const flowforge::FlowGraph& graph) : graph_(&graph)
    {
        for (const auto operation : {
                 flowforge::ENodeOperation::START,
                 flowforge::ENodeOperation::BRANCH,
                 flowforge::ENodeOperation::SEQUENCE
             })
        {
            palette_.push_back({typeId(operation), flowforge::toString(operation), "Flow"});
        }
    }

    node_graph::GraphNodePresentation FlowGraphPresentation::node(graph::NodeId id) const noexcept
    {
        const auto* value = graph_->findNodeById(id);
        return {value ? std::string_view{value->name()} : std::string_view{"Unknown Flow Node"}};
    }

    node_graph::GraphPinPresentation FlowGraphPresentation::pin(graph::PinId id) const noexcept
    {
        const auto* value = graph_->findPin(id);
        return {value ? std::string_view{value->name()} : std::string_view{"Unknown Pin"}};
    }

    std::span<const node_graph::GraphPaletteEntry> FlowGraphPresentation::palette() const noexcept
    {
        return palette_;
    }
} // namespace lux::editor::flow_graph
