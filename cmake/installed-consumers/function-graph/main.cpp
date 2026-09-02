#include <lux/engine/function/graph/GraphLayout.hpp>

int main()
{
    lux::graph::GraphTopology topology;
    const auto node = topology.addNode(lux::graph::NodeTypeId{1U});
    if (!node)
        return 1;
    lux::graph::GraphLayout layout;
    return layout.set(*node, lux::graph::GraphNodeLayout{1.0F, 2.0F, true}) ? 0 : 2;
}
