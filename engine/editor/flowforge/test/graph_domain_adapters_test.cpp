#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/editor/flowforge/FlowGraphEditorAdapter.hpp>
#include <lux/engine/editor/material/MaterialGraphEditorAdapter.hpp>
#include <lux/engine/editor/node_graph/GraphEditingSession.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/material/graph/Node.hpp>

#include <cassert>

namespace
{
    [[nodiscard]] lux::graph::PinId pin(
        const lux::graph::GraphTopology& topology,
        lux::graph::NodeId owner,
        lux::graph::EPinDirection direction,
        std::size_t ordinal = 0U
    )
    {
        std::size_t current{};
        for (const auto& value : topology.pins())
            if (value.owner == owner && value.direction == direction && current++ == ordinal)
                return value.id;
        return {};
    }
}

int main()
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::node_graph;

    material::MaterialGraph material_source;
    material_graph::MaterialGraphDocument material_document{material_source};
    material_graph::MaterialGraphRules material_rules{material_source};
    material_graph::MaterialGraphPresentation material_presentation{material_source};
    GraphEditingSession material_session{material_document, material_rules};
    const auto constant_type = graph::NodeTypeId{
        static_cast<std::uint64_t>(material::EMatNodeKind::CONSTANT) + 1U
    };
    const auto output_type = graph::NodeTypeId{
        static_cast<std::uint64_t>(material::EMatNodeKind::OUTPUT_SURFACE) + 1U
    };
    assert(material_session.apply(AddNodeIntent{constant_type, {}}));
    const auto constant = material_session.selectedNode();
    assert(material_session.apply(AddNodeIntent{output_type, {}}));
    const auto output = material_session.selectedNode();
    assert(material_session.apply(ConnectIntent{
        pin(material_source.topology(), constant, graph::EPinDirection::OUTPUT),
        pin(material_source.topology(), output, graph::EPinDirection::INPUT)
    }));
    assert(!material_presentation.node(constant).title.empty());
    assert(!material_presentation.palette().empty());

    flowforge::FlowGraph flow_source;
    flow_graph::FlowGraphDocument flow_document{flow_source};
    flow_graph::FlowGraphRules flow_rules{flow_source};
    flow_graph::FlowGraphPresentation flow_presentation{flow_source};
    GraphEditingSession flow_session{flow_document, flow_rules};
    const auto start_type = graph::NodeTypeId{
        static_cast<std::uint64_t>(flowforge::ENodeOperation::START) + 1U
    };
    const auto sequence_type = graph::NodeTypeId{
        static_cast<std::uint64_t>(flowforge::ENodeOperation::SEQUENCE) + 1U
    };
    assert(flow_session.apply(AddNodeIntent{start_type, {}}));
    const auto start = flow_session.selectedNode();
    assert(flow_session.apply(AddNodeIntent{sequence_type, {}}));
    const auto sequence = flow_session.selectedNode();
    assert(flow_session.apply(ConnectIntent{
        pin(flow_source.topology(), start, graph::EPinDirection::OUTPUT),
        pin(flow_source.topology(), sequence, graph::EPinDirection::INPUT)
    }));

    const auto pin_count = flow_source.topology().pins().size();
    assert(flow_session.apply(InvokeNodeActionIntent{
        sequence,
        static_cast<std::uint64_t>(flow_graph::EFlowGraphNodeAction::ADD_SEQUENCE_OUTPUT)
    }));
    assert(flow_source.topology().pins().size() == pin_count + 1U);
    const auto dynamic_pin = flow_source.topology().pins().back().id;
    assert(flow_session.undo());
    assert(flow_source.topology().pins().size() == pin_count);
    assert(flow_session.redo());
    assert(flow_source.topology().pins().size() == pin_count + 1U);
    assert(flow_source.topology().findPin(dynamic_pin) != nullptr);
    assert(!flow_presentation.node(sequence).title.empty());
    assert(flow_presentation.palette().size() == 3U);
    return 0;
}
