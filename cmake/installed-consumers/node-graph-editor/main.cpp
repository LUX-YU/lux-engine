#include <lux/engine/editor/node_graph/GraphTypes.hpp>

int main()
{
    const lux::editor::node_graph::GraphNodeRef node{42U};
    return node.valid() ? 0 : 1;
}
