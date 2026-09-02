#include <lux/engine/editor/node_graph/GraphIntent.hpp>

int main()
{
    const lux::editor::node_graph::RemoveNodeIntent intent{lux::graph::NodeId{42U}};
    return intent.node.valid() ? 0 : 1;
}
