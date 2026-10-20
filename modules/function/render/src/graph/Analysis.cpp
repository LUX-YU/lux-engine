#include <lux/engine/render/graph/Analysis.hpp>

#include <lux/engine/render/graph/Range.hpp>
#include <queue>

namespace lux::render
{
    namespace
    {
        using detail::Box;
        using detail::graphError;
        using Edges = std::vector<std::vector<std::uint32_t>>;

        struct Write final
        {
            GraphPassId pass;
            const GraphResourceUse* use;
            std::vector<Box> ranges;
        };

        void addEdge(Edges& edges, GraphPassId before, GraphPassId after) noexcept
        {
            if (before == after)
            {
                return;
            }
            auto& list = edges[before.index];
            if (std::find(list.begin(), list.end(), after.index) == list.end())
            {
                list.push_back(after.index);
            }
        }

        RenderResult<std::vector<GraphPassId>> topological(const Edges& edges, const std::vector<bool>& live) noexcept
        {
            std::vector<std::uint32_t> degree(edges.size());
            std::size_t count = 0;
            for (std::uint32_t from = 0; from < edges.size(); ++from)
            {
                if (!live[from])
                {
                    continue;
                }
                ++count;
                for (auto to : edges[from])
                {
                    if (live[to])
                    {
                        ++degree[to];
                    }
                }
            }
            std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> ready;
            for (std::uint32_t i = 0; i < edges.size(); ++i)
            {
                if (live[i] && degree[i] == 0)
                {
                    ready.push(i);
                }
            }
            std::vector<GraphPassId> order;
            order.reserve(count);
            while (!ready.empty())
            {
                const auto next = ready.top();
                ready.pop();
                order.push_back({next});
                for (auto to : edges[next])
                {
                    if (live[to] && --degree[to] == 0)
                    {
                        ready.push(to);
                    }
                }
            }
            if (order.size() != count)
            {
                // A Kahn remainder can contain innocent downstream nodes. Follow
                // remaining predecessors P times to report an actual cycle member.
                auto cursor = static_cast<std::uint32_t>(
                    std::find_if(degree.begin(), degree.end(), [](auto value) { return value != 0; }) - degree.begin()
                );
                for (std::size_t step = 0; step < edges.size(); ++step)
                {
                    for (std::uint32_t from = 0; from < edges.size(); ++from)
                    {
                        const bool is_predecessor =
                            degree[from] != 0 &&
                            std::find(edges[from].begin(), edges[from].end(), cursor) != edges[from].end();
                        if (is_predecessor)
                        {
                            cursor = from;
                            break;
                        }
                    }
                }
                return cxx::unexpected(graphError(kGraphDependencyCycle, {cursor}));
            }
            return order;
        }

        void removeCovered(std::vector<Box>& remaining, const Box& cover) noexcept
        {
            std::vector<Box> next;
            for (const auto& region : remaining)
            {
                detail::subtract(region, cover, next);
            }
            remaining = std::move(next);
        }

        bool initiallyAvailable(const GraphResource& resource, const VGraphRange& range) noexcept
        {
            auto remaining = detail::boxes(range);
            for (const auto& initial : resource.initial_ranges)
            {
                for (const auto& box : detail::boxes(initial))
                {
                    removeCovered(remaining, box);
                }
            }
            return remaining.empty();
        }

        struct LifetimeCell final
        {
            Box box;
            std::uint32_t first;
            std::uint32_t last;
        };

        void extendCells(std::vector<LifetimeCell>& cells, const Box& range, std::uint32_t position) noexcept
        {
            std::vector<LifetimeCell> next;
            for (const auto& cell : cells)
            {
                const auto overlap = detail::intersection(cell.box, range);
                if (!overlap)
                {
                    next.push_back(cell);
                    continue;
                }
                std::vector<Box> remainder;
                detail::subtract(cell.box, range, remainder);
                for (const auto& box : remainder)
                {
                    next.push_back({box, cell.first, cell.last});
                }
                next.push_back({*overlap, cell.first, std::max(cell.last, position)});
            }
            cells = std::move(next);
        }
    } // namespace

    RenderResult<GraphAnalysis> analyze(const RenderGraphDefinition& definition) noexcept
    {
        const auto resources = definition.resources();
        const auto passes = definition.passes();
        GraphAnalysis result;
        Edges edges(passes.size());
        Edges predecessors(passes.size());
        for (const auto& dependency : definition.dependencies())
        {
            addEdge(edges, dependency.before, dependency.after);
            predecessors[dependency.after.index].push_back(dependency.before.index);
        }
        if (auto valid = topological(edges, std::vector<bool>(passes.size(), true)); !valid)
        {
            return cxx::unexpected(valid.error());
        }

        // Build once from actual uses. No resource-by-all-passes scanning or
        // runtime name lookup. Versions are compared values, never sparse indices.
        std::vector<std::vector<Write>> writers(resources.size());
        for (std::uint32_t p = 0; p < passes.size(); ++p)
        {
            for (const auto& use : passes[p].uses)
            {
                if (detail::writes(use.access))
                {
                    writers[use.version.resource.index].push_back({{p}, &use, detail::boxes(use.range)});
                }
            }
        }
        std::vector<bool> live(passes.size());
        std::vector<GraphPassId> work;
        work.reserve(passes.size());
        auto retain = [&](GraphPassId pass)
        {
            if (!live[pass.index])
            {
                live[pass.index] = true;
                work.push_back(pass);
            }
        };

        auto resolve = [&](GraphVersion version, const VGraphRange& range, GraphPassId consumer, bool fallback
                       ) -> RenderResult<void>
        {
            const auto& resource = resources[version.resource.index];
            const bool image = std::holds_alternative<GraphImageDesc>(resource.description);
            auto remaining = detail::boxes(range);
            if (version.number == 0)
            {
                const bool has_initial_contract =
                    resource.origin == EGraphOrigin::IMPORTED || resource.origin == EGraphOrigin::HISTORY_PREVIOUS;
                if (!has_initial_contract)
                {
                    return cxx::unexpected(graphError(kGraphUndefinedRead, consumer, version.resource));
                }
                if (!initiallyAvailable(resource, range))
                {
                    return cxx::unexpected(graphError(kGraphMissingInitial, consumer, version.resource));
                }
                for (const auto& box : remaining)
                {
                    result.versions.push_back({version, detail::toRange(box, image), {}, consumer, fallback});
                }
                return {};
            }
            struct Piece final
            {
                Box box;
                GraphPassId writer;
            };
            std::vector<Piece> pieces;
            for (const auto& writer : writers[version.resource.index])
            {
                if (writer.use->version.number != version.number)
                {
                    continue;
                }
                for (const auto& requested : detail::boxes(range))
                {
                    for (const auto& written : writer.ranges)
                    {
                        const auto overlap = detail::intersection(requested, written);
                        if (!overlap)
                        {
                            continue;
                        }
                        for (const auto& previous : pieces)
                        {
                            if (detail::intersection(previous.box, *overlap))
                            {
                                return cxx::unexpected(graphError(
                                    kGraphAmbiguousProducer,
                                    writer.pass,
                                    version.resource,
                                    previous.writer.index
                                ));
                            }
                        }
                        if (writer.pass == consumer)
                        {
                            return cxx::unexpected(graphError(kGraphUndefinedRead, consumer, version.resource));
                        }
                        pieces.push_back({*overlap, writer.pass});
                        removeCovered(remaining, *overlap);
                    }
                }
            }
            if (!remaining.empty())
            {
                return cxx::unexpected(graphError(kGraphUndefinedRead, consumer, version.resource, version.number));
            }
            for (const auto& piece : pieces)
            {
                retain(piece.writer);
                const auto part = detail::toRange(piece.box, image);
                result.versions.push_back({version, part, piece.writer, consumer, fallback});
                if (consumer.index != kGraphInvalidIndex)
                {
                    addEdge(edges, piece.writer, consumer);
                    result.hazards.push_back(
                        {piece.writer,
                         consumer,
                         version.resource,
                         part,
                         EGraphHazard::RAW,
                         version.number,
                         version.number}
                    );
                }
            }
            return {};
        };

        for (const auto& output : definition.outputs())
        {
            if (auto valid = resolve(output.version, output.range, {}, false); !valid)
            {
                return cxx::unexpected(valid.error());
            }
        }
        for (std::uint32_t p = 0; p < passes.size(); ++p)
        {
            if (passes[p].side_effect)
            {
                retain({p});
            }
        }
        for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
        {
            const auto pass = work[cursor];
            for (auto before : predecessors[pass.index])
            {
                retain({before});
            }
            for (const auto& use : passes[pass.index].uses)
            {
                const auto resource_id = use.version.resource;
                if (detail::reads(use.access))
                {
                    auto input = use.version;
                    if (detail::writes(use.access))
                    {
                        input.number = use.input_version;
                    }
                    const bool use_fallback = use.access == EGraphAccess::READ && use.fallback &&
                                              !initiallyAvailable(resources[resource_id.index], use.range);
                    if (use_fallback)
                    {
                        input = *use.fallback;
                    }
                    if (auto valid = resolve(input, use.range, pass, use_fallback); !valid)
                    {
                        return cxx::unexpected(valid.error());
                    }
                }
                const bool needs_fallback = detail::writes(use.access) && passes[pass.index].conditional;
                if (needs_fallback)
                {
                    if (!use.fallback)
                    {
                        return cxx::unexpected(graphError(kGraphMissingFallback, pass, resource_id));
                    }
                    if (auto valid = resolve(*use.fallback, use.range, pass, true); !valid)
                    {
                        return cxx::unexpected(valid.error());
                    }
                }
            }
        }

        std::vector<std::vector<std::size_t>> reads(resources.size());
        for (std::size_t i = 0; i < result.versions.size(); ++i)
        {
            reads[result.versions[i].version.resource.index].push_back(i);
        }

        // Only retained writers participate in physical hazards. A discarded
        // overwrite is not a data producer and must not keep a dead branch alive.
        for (std::uint32_t r = 0; r < writers.size(); ++r)
        {
            const bool image = std::holds_alternative<GraphImageDesc>(resources[r].description);
            const auto& list = writers[r];
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                const auto& a = list[i];
                if (!live[a.pass.index])
                {
                    continue;
                }
                for (std::size_t j = 0; j < i; ++j)
                {
                    const auto& b = list[j];
                    if (!live[b.pass.index])
                    {
                        continue;
                    }
                    for (const auto& ar : a.ranges)
                    {
                        for (const auto& br : b.ranges)
                        {
                            const auto overlap = detail::intersection(ar, br);
                            if (!overlap)
                            {
                                continue;
                            }
                            if (a.use->version.number == b.use->version.number)
                            {
                                return cxx::unexpected(graphError(kGraphAmbiguousProducer, a.pass, {r}, b.pass.index));
                            }
                            const auto& before = a.use->version.number < b.use->version.number ? a : b;
                            const auto& after = a.use->version.number < b.use->version.number ? b : a;
                            addEdge(edges, before.pass, after.pass);
                            result.hazards.push_back(
                                {before.pass,
                                 after.pass,
                                 {r},
                                 detail::toRange(*overlap, image),
                                 EGraphHazard::WAW,
                                 before.use->version.number,
                                 after.use->version.number}
                            );
                        }
                    }
                }
                for (auto index : reads[r])
                {
                    const auto& read = result.versions[index];
                    if (read.version.number >= a.use->version.number || read.consumer == a.pass)
                    {
                        continue;
                    }
                    for (const auto& rr : detail::boxes(read.range))
                    {
                        for (const auto& wr : a.ranges)
                        {
                            const auto overlap = detail::intersection(rr, wr);
                            if (!overlap)
                            {
                                continue;
                            }
                            if (read.consumer.index == kGraphInvalidIndex)
                            {
                                return cxx::unexpected(graphError(kGraphConflictingOutput, a.pass, {r}));
                            }
                            addEdge(edges, read.consumer, a.pass);
                            result.hazards.push_back(
                                {read.consumer,
                                 a.pass,
                                 {r},
                                 detail::toRange(*overlap, image),
                                 EGraphHazard::WAR,
                                 read.version.number,
                                 a.use->version.number}
                            );
                        }
                    }
                }
            }
        }
        auto order = topological(edges, live);
        if (!order)
        {
            return cxx::unexpected(order.error());
        }
        result.execution_order = std::move(*order);
        const auto end = static_cast<std::uint32_t>(result.execution_order.size());
        std::vector<std::uint32_t> positions(passes.size(), kGraphInvalidIndex);
        for (std::uint32_t i = 0; i < end; ++i)
        {
            positions[result.execution_order[i].index] = i;
        }
        for (std::uint32_t from = 0; from < edges.size(); ++from)
        {
            if (!live[from])
            {
                continue;
            }
            std::sort(edges[from].begin(), edges[from].end());
            for (auto to : edges[from])
            {
                if (live[to])
                {
                    result.dependencies.push_back({{from}, {to}});
                }
            }
        }

        // Partition each actually used version into exact lifetime ranges. No
        // original pass index is used as a time coordinate. Initial-only unused
        // ranges and dead resources never enter the returned plan.
        for (std::uint32_t r = 0; r < resources.size(); ++r)
        {
            const bool image = std::holds_alternative<GraphImageDesc>(resources[r].description);
            auto appendLifetimes = [&](std::uint32_t version, std::vector<LifetimeCell> cells)
            {
                for (auto index : reads[r])
                {
                    const auto& read = result.versions[index];
                    if (read.version.number != version)
                    {
                        continue;
                    }
                    const auto position =
                        read.consumer.index == kGraphInvalidIndex ? end : positions[read.consumer.index];
                    for (const auto& box : detail::boxes(read.range))
                    {
                        extendCells(cells, box, position);
                    }
                }
                for (const auto& cell : cells)
                {
                    result.lifetimes.push_back(
                        {{{r}, version}, detail::toRange(cell.box, image), resources[r].origin, cell.first, cell.last}
                    );
                }
            };
            std::vector<LifetimeCell> initial;
            for (auto index : reads[r])
            {
                const auto& read = result.versions[index];
                if (read.version.number != 0)
                {
                    continue;
                }
                auto uncovered = detail::boxes(read.range);
                for (const auto& cell : initial)
                {
                    removeCovered(uncovered, cell.box);
                }
                for (const auto& box : uncovered)
                {
                    initial.push_back({box, 0, 0});
                }
            }
            appendLifetimes(0, std::move(initial));
            for (const auto& writer : writers[r])
            {
                if (!live[writer.pass.index])
                {
                    continue;
                }
                std::vector<LifetimeCell> cells;
                const auto position = positions[writer.pass.index];
                for (const auto& box : writer.ranges)
                {
                    cells.push_back({box, position, position});
                }
                appendLifetimes(writer.use->version.number, std::move(cells));
            }
        }
        return result;
    }
} // namespace lux::render
