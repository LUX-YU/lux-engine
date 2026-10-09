#pragma once

#include <lux/engine/function/graph/GraphTypes.hpp>

#include <queue>
#include <span>
#include <unordered_set>

namespace lux::flowforge::detail
{
    // Both the live graph query and immutable suspension projection use this direct-edge walk.
    // Successors synchronously calls emit for its ordered outgoing edges; visit returns false to stop.
    template <class Successors, class Visit>
    void visitExecution(
        std::span<const graph::NodeId> roots,
        Successors&& successors,
        Visit&& visit,
        graph::NodeId excluded = {}
    ) noexcept
    {
        std::queue<graph::NodeId> pending;
        std::unordered_set<graph::NodeId> visited;
        if (excluded.valid())
        {
            visited.insert(excluded);
        }
        for (const auto root : roots)
        {
            if (root.valid())
            {
                pending.push(root);
            }
        }
        while (!pending.empty())
        {
            const auto id = pending.front();
            pending.pop();
            if (!visited.insert(id).second)
            {
                continue;
            }
            if (!visit(id))
            {
                return;
            }
            successors(id, [&](graph::NodeId next) noexcept { pending.push(next); });
        }
    }
} // namespace lux::flowforge::detail
