#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <memory>

int main()
{
    lux::material::MaterialGraph graph;
    const auto value = graph.addNode(std::make_unique<lux::material::ConstantNode>());
    const auto output = graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
    return graph.connect(
        value,
        0U,
        output,
        static_cast<std::uint32_t>(lux::material::EMaterialAttribute::BASE_COLOR)
    ) ? 0 : 1;
}
