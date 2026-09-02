#include <lux/engine/editor/flowforge/FlowGraphEditorAdapter.hpp>
#include <lux/engine/editor/material/MaterialGraphEditorAdapter.hpp>

int main()
{
    lux::material::MaterialGraph material;
    lux::editor::material_graph::MaterialGraphDocument material_document{material};
    lux::editor::material_graph::MaterialGraphPresentation material_presentation{material};
    lux::flowforge::FlowGraph flow;
    lux::editor::flow_graph::FlowGraphDocument flow_document{flow};
    lux::editor::flow_graph::FlowGraphPresentation flow_presentation{flow};
    return material_document.topology().nodes().empty() &&
        flow_document.topology().nodes().empty() &&
        !material_presentation.palette().empty() &&
        !flow_presentation.palette().empty() ? 0 : 1;
}
