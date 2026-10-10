#include <lux/engine/render/graph/Plan.hpp>

#include <algorithm>
#include <functional>
#include <queue>
#include <utility>

namespace lux::render
{
    namespace
    {
        using Adjacency = std::vector<std::vector<std::uint32_t>>;

        void canonicalize(Adjacency& edges) noexcept
        {
            for (auto& successors : edges)
            {
                std::sort(successors.begin(), successors.end());
                successors.erase(std::unique(successors.begin(), successors.end()), successors.end());
            }
        }

        RenderResult<std::vector<GraphPassId>> sortPasses(const Adjacency& edges) noexcept
        {
            std::vector<std::size_t> indegrees(edges.size(), 0);
            for (const auto& successors : edges)
            {
                for (auto successor : successors)
                {
                    ++indegrees[successor];
                }
            }
            std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> ready;
            for (std::size_t index = 0; index < edges.size(); ++index)
            {
                if (indegrees[index] == 0)
                {
                    ready.push(static_cast<std::uint32_t>(index));
                }
            }
            std::vector<GraphPassId> order;
            order.reserve(edges.size());
            while (!ready.empty())
            {
                const auto current = ready.top();
                ready.pop();
                order.push_back(GraphPassId{current + 1});
                for (auto successor : edges[current])
                {
                    if (--indegrees[successor] == 0)
                    {
                        ready.push(successor);
                    }
                }
            }
            if (order.size() != edges.size())
            {
                return cxx::unexpected(RenderError{kGraphCycle, {edges.size() - order.size()}});
            }
            return order;
        }

        struct ResourceAccesses
        {
            std::optional<std::uint32_t> writer;
            std::vector<std::uint32_t> readers;
        };
    }

    CompiledGraphPlan::CompiledGraphPlan(
        RenderGraphDefinition definition,
        std::vector<GraphPassId> order,
        std::vector<GraphDependency> dependencies,
        std::vector<std::optional<GraphResourceLifetime>> lifetimes,
        std::vector<GraphResourceId> imports
    ) noexcept : definition_(std::move(definition)), order_(std::move(order)),
        dependencies_(std::move(dependencies)), lifetimes_(std::move(lifetimes)), imports_(std::move(imports))
    {
    }

    RenderResult<CompiledGraphPlan> CompiledGraphPlan::compile(const RenderGraphDefinition& definition) noexcept
    {
        Adjacency edges(definition.passes().size());
        for (const auto& dependency : definition.dependencies())
        {
            edges[dependency.before.value() - 1].push_back(dependency.after.value() - 1);
        }
        // Preserve V1's explicit-order-first interpretation of mutable resource
        // versions. Equal-ready passes use declaration position, never names.
        auto logical_order = sortPasses(edges);
        if (!logical_order)
        {
            return cxx::unexpected(logical_order.error());
        }
        std::vector<ResourceAccesses> accesses(definition.resources().size());
        for (auto pass : *logical_order)
        {
            const auto index = pass.value() - 1;
            for (const auto& use : definition.passes()[index].uses)
            {
                const auto resource_index = use.resource.value() - 1;
                auto& access = accesses[resource_index];
                const bool is_read = use.access != EGraphAccess::WRITE;
                const bool is_write = use.access != EGraphAccess::READ;
                const bool is_uninitialized_read = is_read && !access.writer &&
                    definition.resources()[resource_index].origin == EGraphResourceOrigin::TRANSIENT;
                if (is_uninitialized_read)
                {
                    return cxx::unexpected(RenderError{kGraphMissingProducer, {pass.value(), use.resource.value()}});
                }
                if (access.writer)
                {
                    edges[*access.writer].push_back(index); // RAW or WAW.
                }
                if (is_write)
                {
                    // All reads of the old value must finish before overwrite.
                    // This includes imported initial values and READ_WRITE.
                    for (auto reader : access.readers)
                    {
                        edges[reader].push_back(index); // WAR.
                    }
                    access.readers.clear();
                    access.writer = index;
                }
                else
                {
                    access.readers.push_back(index);
                }
            }
        }
        canonicalize(edges);
        auto order = sortPasses(edges);
        if (!order)
        {
            return cxx::unexpected(order.error());
        }
        std::vector<std::optional<GraphResourceLifetime>> lifetimes(definition.resources().size());
        for (std::size_t position = 0; position < order->size(); ++position)
        {
            for (const auto& use : definition.passes()[(*order)[position].value() - 1].uses)
            {
                auto& interval = lifetimes[use.resource.value() - 1];
                const auto current = static_cast<std::uint32_t>(position);
                if (!interval)
                {
                    interval = GraphResourceLifetime{current, current};
                }
                interval->last_pass = current;
            }
        }
        std::vector<GraphDependency> dependencies;
        for (std::size_t index = 0; index < edges.size(); ++index)
        {
            for (auto successor : edges[index])
            {
                dependencies.push_back({
                    GraphPassId{static_cast<std::uint32_t>(index + 1)}, GraphPassId{successor + 1}
                });
            }
        }
        std::vector<GraphResourceId> imports;
        for (std::size_t index = 0; index < definition.resources().size(); ++index)
        {
            if (definition.resources()[index].origin == EGraphResourceOrigin::IMPORTED)
            {
                imports.push_back(GraphResourceId{static_cast<std::uint32_t>(index + 1)});
            }
        }
        return CompiledGraphPlan{definition, std::move(*order), std::move(dependencies),
            std::move(lifetimes), std::move(imports)
        };
    }
}
