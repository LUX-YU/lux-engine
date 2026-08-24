#include <lux/engine/ecs/detail/ScheduleTopology.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::ecs::detail
{
    namespace
    {
        constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

        [[nodiscard]] bool precedes(
            std::span<const ScheduleTopologyNodeView> nodes,
            std::size_t lhs,
            std::size_t rhs) noexcept
        {
            if (nodes[lhs].phase != nodes[rhs].phase)
                return nodes[lhs].phase < nodes[rhs].phase;
            if (nodes[lhs].sequence != nodes[rhs].sequence)
                return nodes[lhs].sequence < nodes[rhs].sequence;
            return lhs < rhs;
        }

        [[nodiscard]] bool accessesConflict(
            const ISystem::AccessDeclaration& lhs,
            const ISystem::AccessDeclaration& rhs) noexcept
        {
            if (!lhs.complete || !rhs.complete ||
                lhs.structural || rhs.structural)
                return true;

            for (const auto& left : lhs.resources)
                for (const auto& right : rhs.resources)
                    if (left.resource == right.resource &&
                        (left.mode == ISystem::AccessMode::Write ||
                         right.mode == ISystem::AccessMode::Write))
                        return true;
            return false;
        }

        [[nodiscard]] bool hasEdge(
            const std::vector<std::vector<std::size_t>>& outgoing,
            std::size_t from,
            std::size_t to) noexcept
        {
            return std::find(
                       outgoing[from].begin(),
                       outgoing[from].end(),
                       to
                   ) != outgoing[from].end();
        }
    }

    ScheduleTopologyAnalysis analyzeScheduleTopology(std::span<const ScheduleTopologyNodeView> nodes)
    {
        ScheduleTopologyAnalysis analysis;
        const std::size_t node_count = nodes.size();

        const auto lookup = [&](SystemType type) noexcept -> std::size_t
        {
            for (std::size_t index = 0; index < node_count; ++index)
                if (sameSystemType(nodes[index].type, type))
                    return index;
            return kNoIndex;
        };

        // Duplicates should already be rejected by Schedule's mutation gate,
        // but the shared analyser must remain well-defined for every snapshot.
        for (std::size_t index = 0; index < node_count; ++index)
            for (std::size_t previous = 0; previous < index; ++previous)
                if (sameSystemType(nodes[index].type, nodes[previous].type))
                {
                    analysis.report.duplicate.push_back(nodes[index].type);
                    break;
                }

        for (const auto& node : nodes)
            for (const auto requirement : node.prerequisites)
                if (lookup(requirement) == kNoIndex)
                    analysis.report.missing_prereq.emplace_back(
                        node.type,
                        requirement
                    );

        std::vector<std::vector<std::size_t>> outgoing(node_count);
        std::vector<std::size_t> indegree(node_count, 0);
        const auto addEdge = [&](std::size_t from, std::size_t to)
        {
            // A self edge is a real one-node cycle. Edge deduplication keeps
            // A.before(B) + B.after(A) from inflating B's indegree.
            if (hasEdge(outgoing, from, to)) return;
            outgoing[from].push_back(to);
            ++indegree[to];
        };

        for (std::size_t index = 0; index < node_count; ++index)
        {
            for (const auto type : nodes[index].runs_after)
            {
                const auto other = lookup(type);
                if (other == kNoIndex)
                {
                    analysis.report.unknown.push_back(type);
                    continue;
                }
                addEdge(other, index);
            }

            for (const auto type : nodes[index].runs_before)
            {
                const auto other = lookup(type);
                if (other == kNoIndex)
                {
                    analysis.report.unknown.push_back(type);
                    continue;
                }
                addEdge(index, other);
            }
        }

        // Tarjan SCC classification is independent from Kahn's fallback. This
        // reports only nodes that are actually in a cycle, not acyclic nodes
        // whose sole incoming path happens to originate in a cycle.
        std::vector<std::size_t> discovery(node_count, kNoIndex);
        std::vector<std::size_t> lowlink(node_count, 0);
        std::vector<std::size_t> stack;
        std::vector<std::uint8_t> on_stack(node_count, 0);
        std::vector<std::size_t> cyclic_nodes;
        stack.reserve(node_count);
        cyclic_nodes.reserve(node_count);
        std::size_t next_discovery = 0;

        const auto visit = [&](auto&& self, std::size_t node) -> void
        {
            discovery[node] = next_discovery;
            lowlink[node] = next_discovery;
            ++next_discovery;
            stack.push_back(node);
            on_stack[node] = 1;

            for (const auto next : outgoing[node])
            {
                if (discovery[next] == kNoIndex)
                {
                    self(self, next);
                    lowlink[node] = std::min(lowlink[node], lowlink[next]);
                }
                else if (on_stack[next] != 0)
                {
                    lowlink[node] = std::min(lowlink[node], discovery[next]);
                }
            }

            if (lowlink[node] != discovery[node]) return;

            std::size_t component_begin = stack.size();
            while (component_begin > 0)
            {
                --component_begin;
                if (stack[component_begin] == node) break;
            }

            const auto component_size = stack.size() - component_begin;
            const bool cyclic = component_size > 1 || hasEdge(
                outgoing,
                node,
                node
            );
            if (cyclic)
                cyclic_nodes.insert(
                    cyclic_nodes.end(),
                    stack.begin() + static_cast<std::ptrdiff_t>(component_begin),
                    stack.end()
                );

            for (std::size_t index = component_begin;
                 index < stack.size(); ++index)
                on_stack[stack[index]] = 0;
            stack.resize(component_begin);
        };

        for (std::size_t index = 0; index < node_count; ++index)
            if (discovery[index] == kNoIndex)
                visit(visit, index);

        std::sort(
            cyclic_nodes.begin(),
            cyclic_nodes.end(),
            [&](std::size_t lhs, std::size_t rhs)
            {
                return precedes(nodes, lhs, rhs);
            }
        );
        for (const auto index : cyclic_nodes)
            analysis.report.cycle.push_back(nodes[index].type);

        std::vector<std::size_t> ready;
        ready.reserve(node_count);
        for (std::size_t index = 0; index < node_count; ++index)
            if (indegree[index] == 0) ready.push_back(index);

        analysis.order.reserve(node_count);
        while (!ready.empty())
        {
            auto best = ready.begin();
            for (auto candidate = ready.begin() + 1;
                 candidate != ready.end(); ++candidate)
                if (precedes(nodes, *candidate, *best))
                    best = candidate;

            const auto current = *best;
            ready.erase(best);
            analysis.order.push_back(current);
            for (const auto next : outgoing[current])
                if (--indegree[next] == 0) ready.push_back(next);
        }

        if (analysis.order.size() < node_count)
        {
            std::vector<std::size_t> blocked;
            blocked.reserve(node_count - analysis.order.size());
            for (std::size_t index = 0; index < node_count; ++index)
                if (indegree[index] != 0) blocked.push_back(index);
            std::sort(
                blocked.begin(),
                blocked.end(),
                [&](std::size_t lhs, std::size_t rhs)
                {
                    return precedes(nodes, lhs, rhs);
                }
            );
            analysis.order.insert(
                analysis.order.end(),
                blocked.begin(),
                blocked.end()
            );
        }

        for (std::size_t position = 0;
             position < analysis.order.size(); ++position)
        {
            bool start_new = analysis.batches.empty();
            if (!start_new)
            {
                const auto& batch = analysis.batches.back();
                const auto current = analysis.order[position];
                const auto first = analysis.order[batch.first];
                start_new = nodes[current].phase != nodes[first].phase;

                for (std::size_t prior_position = batch.first;
                     !start_new &&
                     prior_position < batch.first + batch.count;
                     ++prior_position)
                {
                    const auto prior = analysis.order[prior_position];
                    start_new = hasEdge(outgoing, prior, current) ||
                                hasEdge(outgoing, current, prior) ||
                                accessesConflict(
                                    nodes[prior].access,
                                    nodes[current].access
                                );
                }
            }

            if (start_new)
                analysis.batches.push_back(
                    Schedule::ExecutionBatch{position, 1}
                );
            else
                ++analysis.batches.back().count;
        }

        return analysis;
    }

} // namespace lux::ecs::detail
