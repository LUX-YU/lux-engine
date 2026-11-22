#include <lux/engine/render/effect/EffectPreparation.hpp>
#include <lux/engine/render/graph/compile/GraphLiveness.hpp>

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
            const ResourceAccess* use;
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

        // Persistent facts for one range proof. Following a skipped write adds its inverse predicate;
        // other ranges and other consumers keep their own path. Zero denotes the unconstrained path.
        struct PredicatePaths final
        {
            struct Fact final
            {
                PassCondition condition;
                std::size_t parent;
            };

            std::vector<Fact> facts;
            std::vector<std::size_t> reads;

            std::optional<bool> known(std::size_t path, PassCondition condition) const noexcept
            {
                while (path)
                {
                    const auto& fact = facts[path - 1];
                    if (fact.condition.source == condition.source && fact.condition.input == condition.input)
                        return fact.condition.expected;
                    path = fact.parent;
                }
                return {};
            }

            std::size_t extend(std::size_t parent, PassCondition condition)
            {
                facts.push_back({condition, parent});
                return facts.size();
            }
        };

        enum class EReadPath : std::uint8_t { PRIMARY, FALLBACK, SKIPPED_WRITE };

        struct Observation final
        {
            GraphPassId consumer;
            EReadPath mode = EReadPath::PRIMARY;
            std::optional<PassCondition> publication;
            std::optional<PassCondition> selection;
        };

        struct ReachingRange final
        {
            ResourceVersion version;
            VGraphRange range;
            std::size_t path;
        };

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

    RenderResult<RenderGraphAnalysis> analyzeRenderGraph(const RenderGraph& definition) noexcept
    {
        return detail::analyzeGraphWithPreparation(definition, nullptr);
    }

    RenderResult<RenderGraphAnalysis>
    detail::analyzeGraphWithPreparation(const RenderGraph& definition, PreparationLiveness* preparation) noexcept
    {
        const auto resources = definition.resources();
        const auto passes = definition.passes();
        RenderGraphAnalysis result;
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
            if (live[pass.index]) return;
            const auto& command = passes[pass.index];
            const bool sequence = command.sequence_first.index != kGraphInvalidIndex;
            const auto first = sequence ? command.sequence_first.index : pass.index;
            const auto last = sequence ? command.sequence_last.index : pass.index;
            for (auto index = first; index <= last; ++index)
            {
                live[index] = true;
                work.push_back({index});
            }
        };

        PredicatePaths paths;
        auto resolve =
            [&](ResourceVersion initial_version, const VGraphRange& initial_range, Observation observation) -> RenderResult<void>
        {
            const auto [consumer, mode, publication, selection] = observation;
            std::size_t initial_path = 0;
            auto guard = consumer.index == kGraphInvalidIndex ? publication : passes[consumer.index].condition;
            if (guard)
            {
                if (mode == EReadPath::SKIPPED_WRITE) guard->expected = !guard->expected;
                initial_path = paths.extend(0, *guard);
            }
            if (selection)
            {
                const auto known = paths.known(initial_path, *selection);
                if (known && *known != selection->expected) return {};
                if (!known) initial_path = paths.extend(initial_path, *selection);
            }
            const bool fallback = mode != EReadPath::PRIMARY;
            std::vector<ReachingRange> pending{{initial_version, initial_range, initial_path}};
            while (!pending.empty())
            {
                auto [version, range, path] = std::move(pending.back());
                pending.pop_back();
                const auto& resource = resources[version.resource.index];
                const bool image = std::holds_alternative<GraphImageDesc>(resource.description);
                auto remaining = detail::boxes(range);
                if (version.number == 0)
                {
                    const bool has_initial_contract = resource.origin == EGraphOrigin::IMPORTED ||
                                                      resource.origin == EGraphOrigin::HISTORY_PREVIOUS ||
                                                      resource.origin == EGraphOrigin::HISTORY_CURRENT;
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
                        paths.reads.push_back(path);
                    }
                    continue;
                }
                struct Piece final
                {
                    Box box;
                    const Write* writer;
                };
                std::vector<Piece> pieces;
                std::optional<std::uint32_t> predecessor;
                bool split_selection = false;
                for (const auto& writer : writers[version.resource.index])
                {
                    if (writer.use->version.number != version.number)
                    {
                        continue;
                    }
                    if (predecessor && *predecessor != writer.use->input_version)
                    {
                        return cxx::unexpected(graphError(kGraphAmbiguousProducer, consumer, version.resource));
                    }
                    predecessor = writer.use->input_version;
                    const auto& producer = passes[writer.pass.index];
                    if (producer.choice_peer.index != kGraphInvalidIndex)
                    {
                        const auto predicate = *producer.condition;
                        const auto selected = paths.known(path, predicate);
                        if (!selected)
                        {
                            auto opposite = predicate;
                            opposite.expected = !opposite.expected;
                            pending.push_back({version, range, paths.extend(path, opposite)});
                            pending.push_back({version, range, paths.extend(path, predicate)});
                            split_selection = true;
                            break;
                        }
                        if (*selected != predicate.expected) continue;
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
                                        previous.writer->pass.index
                                    ));
                                }
                            }
                            pieces.push_back({*overlap, &writer});
                            removeCovered(remaining, *overlap);
                        }
                    }
                }
                if (split_selection) continue;
                if (!remaining.empty())
                {
                    if (!predecessor || *predecessor >= version.number)
                    {
                        return cxx::unexpected(
                            graphError(kGraphUndefinedRead, consumer, version.resource, version.number)
                        );
                    }
                    for (const auto& part : remaining)
                    {
                        pending.push_back({{version.resource, *predecessor}, detail::toRange(part, image), path});
                    }
                }
                for (const auto& piece : pieces)
                {
                    const auto writer = piece.writer->pass;
                    const auto part = detail::toRange(piece.box, image);
                    auto active_path = path;
                    if (auto guard = passes[writer.index].condition)
                    {
                        const auto value = paths.known(path, *guard);
                        if (!value || *value != guard->expected)
                        {
                            const auto& use = *piece.writer->use;
                            const auto previous = use.fallback.value_or(
                                ResourceVersion{version.resource, use.input_version}
                            );
                            if (previous.resource != version.resource || previous.number >= version.number)
                            {
                                return cxx::unexpected(
                                    graphError(kGraphUndefinedRead, consumer, version.resource, version.number)
                                );
                            }
                            auto skipped = *guard;
                            skipped.expected = !skipped.expected;
                            pending.push_back({previous, part, value ? path : paths.extend(path, skipped)});
                            if (value) continue;
                        }
                        if (!value) active_path = paths.extend(path, *guard);
                    }
                    if (writer == consumer)
                        return cxx::unexpected(graphError(kGraphUndefinedRead, consumer, version.resource));
                    retain(writer);
                    result.versions.push_back({version, part, writer, consumer, fallback});
                    paths.reads.push_back(active_path);
                    if (consumer.index != kGraphInvalidIndex)
                    {
                        addEdge(edges, writer, consumer);
                        result.hazards.push_back(
                            {writer,
                             consumer,
                             version.resource,
                             part,
                             EGraphHazard::RAW,
                             version.number,
                             version.number}
                        );
                    }
                }
            }
            return {};
        };

        for (const auto& output : definition.outputs())
        {
            Observation observed{};
            if (output.alternate) observed.selection = output.alternate->primary_when;
            if (auto valid = resolve(output.version, output.range, observed); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            if (output.alternate)
            {
                observed.selection->expected = !observed.selection->expected;
                if (auto valid = resolve(output.alternate->value, output.range, observed); !valid)
                    return cxx::unexpected(valid.error());
            }
        }
        for (const auto& history : definition.histories())
        {
            if (!history.publication)
            {
                continue;
            }
            const auto range = detail::normalize(resources[history.current.index], {});
            Observation sample{{}, EReadPath::PRIMARY, history.publish_when, history.select_primary};
            if (auto valid = resolve(*history.publication, *range, sample); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            if (history.alternate_sample)
            {
                sample.selection->expected = !sample.selection->expected;
                if (auto valid = resolve(*history.alternate_sample, *range, sample); !valid)
                    return cxx::unexpected(valid.error());
            }
        }
        for (std::uint32_t p = 0; p < passes.size(); ++p)
        {
            const bool writes_import = std::ranges::any_of(passes[p].uses, [&](const auto& use) noexcept
            {
                return detail::writes(use.access) &&
                       resources[use.version.resource.index].origin == EGraphOrigin::IMPORTED;
            });
            if (passes[p].side_effect || writes_import)
            {
                retain({p});
            }
        }
        std::vector<std::uint32_t> pending_producers;
        const auto retain_producer = [&](std::uint32_t index) noexcept
        {
            auto& producer = preparation->producers[index];
            if (producer.live) return;
            producer.live = true;
            pending_producers.push_back(index);
        };
        std::size_t producer_cursor = 0;
        for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
        {
            const auto pass = work[cursor];
            if (preparation)
            {
                for (auto owner : preparation->pass_producers[pass.index]) retain_producer(owner);
                while (producer_cursor < pending_producers.size())
                {
                    const auto& producer = preparation->producers[pending_producers[producer_cursor++]];
                    for (auto input : producer.inputs) retain_producer(input);
                    for (auto member : producer.passes) retain(member);
                }
            }
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
                    const bool has_fallback = use.access == EGraphAccess::READ && use.fallback.has_value();
                    // History validity is an instance decision. Retain both possible producers and
                    // their hazards, including when the declaration also supplies initial content.
                    if (has_fallback)
                    {
                        if (auto valid = resolve(*use.fallback, use.range, {pass, EReadPath::FALLBACK}); !valid)
                        {
                            return cxx::unexpected(valid.error());
                        }
                    }
                    const bool has_primary =
                        !has_fallback || initiallyAvailable(resources[resource_id.index], use.range);
                    if (has_primary)
                    {
                        if (auto valid = resolve(input, use.range, {pass, EReadPath::PRIMARY}); !valid)
                        {
                            return cxx::unexpected(valid.error());
                        }
                    }
                }
                if (detail::writes(use.access) && passes[pass.index].condition && use.fallback)
                {
                    if (auto valid = resolve(*use.fallback, use.range, {pass, EReadPath::SKIPPED_WRITE}); !valid)
                        return cxx::unexpected(valid.error());
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
                    if (passes[a.pass.index].choice_peer == b.pass) continue;
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
                    if (const auto guard = passes[a.pass.index].condition)
                    {
                        const auto value = paths.known(paths.reads[index], *guard);
                        if (value && *value != guard->expected) continue;
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
        // A live sequence is scheduled as one interval. Project its external edges onto entry and exit;
        // internal hazards still retain their individual command positions for synchronization.
        std::vector<GraphDependency> sequence_edges;
        for (std::uint32_t from = 0; from < edges.size(); ++from)
        {
            if (!live[from]) continue;
            for (auto to : edges[from])
            {
                if (!live[to]) continue;
                const auto& predecessor = passes[from];
                const auto& successor = passes[to];
                const auto start = successor.sequence_first;
                const auto end = predecessor.sequence_last;
                if (start.index != kGraphInvalidIndex && start == predecessor.sequence_first) continue;
                const GraphPassId before = end.index == kGraphInvalidIndex ? GraphPassId{from} : end;
                const GraphPassId after = start.index == kGraphInvalidIndex ? GraphPassId{to} : start;
                if (before.index != from || after.index != to)
                {
                    sequence_edges.push_back({before, after});
                }
            }
        }
        for (const auto& edge : sequence_edges)
        {
            addEdge(edges, edge.before, edge.after);
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

namespace lux::render::runtime_detail
{
    bool
    sameBlockContract(ConstantBlockContract left, ConstantBlockContract right, rdesc::EShaderTarget target) noexcept
    {
        const bool same_type = left.type == right.type && left.type.name() == right.type.name();
        const bool same_storage = left.bytes == right.bytes && left.alignment == right.alignment;
        if (!same_type || !same_storage || !left.layout || !right.layout)
            return false;
        if (left.layout == right.layout) return true;
        const auto a = left.layout(target);
        const auto b = right.layout(target);
        const auto same_member = [](const ConstantMemberLayout& x, const ConstantMemberLayout& y) noexcept
        {
            return x.path == y.path && x.scalar == y.scalar && x.offset == y.offset && x.rows == y.rows &&
                x.columns == y.columns && x.matrix_order == y.matrix_order && x.matrix_stride == y.matrix_stride &&
                std::ranges::equal(x.array_dimensions, y.array_dimensions);
        };
        const bool same_extent = a.bytes == b.bytes && a.alignment == b.alignment;
        return same_extent && std::ranges::equal(a.members, b.members, same_member);
    }

    RenderResult<RenderGraphAnalysis>
    analyzePreparedGraph(
        const RenderGraph& source, PreparationDefinition& definition, rdesc::EShaderTarget target
    ) noexcept
    {
        for (const auto& pass : source.passes())
        {
            const auto* raster = std::get_if<RasterWork>(&pass.command);
            const auto* stage = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
            if (!stage) continue;
            const bool invalid_textures = stage->shared_textures &&
                stage->shared_textures->index >= definition.texture_sets.size();
            const bool invalid_meshes = stage->mesh_geometry && stage->mesh_geometry->index >= definition.mesh_sets.size();
            const bool invalid_materials = stage->shared_materials &&
                stage->shared_materials->index >= definition.material_sets.size();
            if (invalid_materials)
                return cxx::unexpected(RenderError{kGraphUnknownResource,
                    {kGraphInvalidIndex, stage->shared_materials->index}});
            if (invalid_textures || invalid_meshes)
            {
                const auto index = invalid_textures ? stage->shared_textures->index : stage->mesh_geometry->index;
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, index}});
            }
        }
        const auto count = static_cast<std::uint32_t>(definition.producers.size());
        if (!count && !definition.buffers.empty())
            return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex}});
        if (!count && definition.inline_copies.empty()) return analyzeRenderGraph(source);
        std::vector<bool> inline_writers(source.passes().size());
        for (const auto& copy : definition.inline_copies)
        {
            if (copy.pass.index >= inline_writers.size())
                return cxx::unexpected(RenderError{kGraphUnknownPass, {copy.pass.index}});
            const auto* parameters = source.passes()[copy.pass.index].bindings().parameters;
            const auto& block = copy.input.block;
            const bool matches_block = parameters && sameBlockContract(parameters->inline_constant, block, target);
            if (!copy.input.bind || !matches_block)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {copy.pass.index}});
            if (inline_writers[copy.pass.index])
                return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {copy.pass.index}});
            inline_writers[copy.pass.index] = true;
        }
        if (!count) return analyzeRenderGraph(source);
        detail::PreparationLiveness liveness;
        liveness.producers.resize(count);
        liveness.pass_producers.resize(source.passes().size());
        std::vector<std::uint32_t> writers(definition.cpu_data.size(), kGraphInvalidIndex);
        std::vector<std::uint32_t> buffer_writers(source.resources().size(), kGraphInvalidIndex);
        std::vector<std::vector<std::uint32_t>> successors(count);
        std::vector<std::uint32_t> indegree(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& declaration = definition.producers[i].declaration;
            auto& producer = liveness.producers[i];
            producer.passes = declaration.passes;
            for (auto input : declaration.storage_inputs)
            {
                if (input.index >= definition.snapshots.size())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, input.index}});
            }
            for (auto input : declaration.program_storage_inputs)
            {
                if (std::ranges::find(declaration.storage_inputs, input) == declaration.storage_inputs.end())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, input.index}});
                if (std::ranges::count(declaration.program_storage_inputs, input) != 1)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, input.index}});
            }
            for (auto output : declaration.outputs)
            {
                if (output.index >= writers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, output.index}});
                auto& writer = writers[output.index];
                if (writer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, output.index}});
                writer = i;
            }
            for (auto output : declaration.gpu_buffers)
            {
                const auto buffer = std::ranges::find(definition.buffers, output, &PreparationDefinition::Buffer::resource);
                if (buffer == definition.buffers.end() || buffer->node != definition.producers[i].node)
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, output.index}});
                auto& writer = buffer_writers[output.index];
                if (writer != kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {kGraphInvalidIndex, output.index}});
                writer = i;
            }
            for (auto pass : declaration.inline_blocks)
            {
                if (pass.index >= inline_writers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {pass.index}});
                const auto* parameters = source.passes()[pass.index].bindings().parameters;
                if (!parameters || !parameters->inline_constant.layout)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index}});
                if (inline_writers[pass.index])
                    return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {pass.index}});
                inline_writers[pass.index] = true;
                if (std::ranges::find(producer.passes, pass) == producer.passes.end())
                    producer.passes.push_back(pass);
            }
            if (producer.passes.empty() && declaration.outputs.empty() && declaration.gpu_buffers.empty())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {i}});
            for (auto pass : producer.passes)
            {
                if (pass.index >= liveness.pass_producers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownPass, {pass.index}});
                liveness.pass_producers[pass.index].push_back(i);
                const auto peer = source.passes()[pass.index].choice_peer;
                if (peer.index != kGraphInvalidIndex) liveness.pass_producers[peer.index].push_back(i);
            }
        }
        for (const auto& buffer : definition.buffers)
        {
            if (buffer_writers[buffer.resource.index] == kGraphInvalidIndex)
                return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, buffer.resource.index}});
        }
        for (std::uint32_t pass = 0; pass < source.passes().size(); ++pass)
        {
            auto& producers = liveness.pass_producers[pass];
            for (const auto& access : source.passes()[pass].uses)
            {
                const bool frame_buffer_fallback = access.fallback &&
                    source.resources()[access.fallback->resource.index].origin == EGraphOrigin::FRAME_BUFFER;
                if (frame_buffer_fallback)
                    return cxx::unexpected(RenderError{
                        kGraphInvalidDescriptor, {pass, access.fallback->resource.index}
                    });
                const auto resource = access.version.resource.index;
                const auto writer = buffer_writers[resource];
                if (writer == kGraphInvalidIndex) continue;
                const bool gpu_written = source.resources()[resource].origin == EGraphOrigin::FRAME_BUFFER;
                const bool invalid_access = gpu_written ? !access.whole_resource : access.access != EGraphAccess::READ;
                if (invalid_access || access.fallback)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass, resource}});
                if (std::ranges::find(producers, writer) == producers.end()) producers.push_back(writer);
            }
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& inputs = definition.producers[i].declaration.inputs;
            for (auto input : inputs)
            {
                if (input.index >= writers.size())
                    return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, input.index}});
                const auto writer = writers[input.index];
                if (writer == kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, input.index}});
                liveness.producers[i].inputs.push_back(writer);
                successors[writer].push_back(i);
                ++indegree[i];
            }
        }
        std::vector<std::uint32_t> order;
        order.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (indegree[i] == 0) order.push_back(i);
        }
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            for (auto successor : successors[order[i]])
            {
                if (--indegree[successor] == 0) order.push_back(successor);
            }
        }
        if (order.size() != count) return cxx::unexpected(RenderError{kGraphDependencyCycle});
        auto analysis = detail::analyzeGraphWithPreparation(source, &liveness);
        if (!analysis) return cxx::unexpected(analysis.error());
        for (auto producer : order)
        {
            if (liveness.producers[producer].live) definition.live_order.push_back(producer);
        }
        return analysis;
    }
} // namespace lux::render::runtime_detail
