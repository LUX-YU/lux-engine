#pragma once

#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/function/graph/GraphTypes.hpp>

#include <vector>

namespace lux::flowforge
{
    class FlowGraph;

    // Disposable compilation facts for the supplied graph. Iterative traversal follows only direct
    // execution edges, not function calls or data dependencies. Unknown/unconnected starts are empty.
    // Returned IDs do not own or borrow nodes; editing the graph invalidates the analysis they express.
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC std::vector<graph::NodeId> reachableExecution(
        const FlowGraph&,
        graph::PinId start
    ) noexcept;

    // The existing compiler's branch-region rule: first common reachable node in breadth-first order,
    // seeding the true leg before the false leg and excluding the branch itself. This is not a general
    // post-dominance proof. The caller supplies its two declared execution legs; this algorithm
    // does not depend on a concrete BranchNode C++ type. Foreign legs or disjoint paths return no ID.
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC graph::NodeId findBranchMerge(
        const FlowGraph&,
        graph::NodeId branch,
        graph::PinId true_leg,
        graph::PinId false_leg
    ) noexcept;
} // namespace lux::flowforge
