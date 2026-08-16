#include <lux/engine/authoring/flowforge/FlowGraph.hpp>

namespace lux::flowforge
{

const std::vector<NodeStorage>& FlowGraph::nodes() const
{
    return nodes_.values();
}

} // namespace lux::flowforge
