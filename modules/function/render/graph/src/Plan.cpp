#include <lux/engine/render/graph/Plan.hpp>

#include <algorithm>
#include <sstream>

namespace lux::render
{
    namespace
    {
        constexpr auto kAmbiguous = kGraphAmbiguousProducer;
        constexpr auto kScope = kGraphScopeConflict;
        constexpr auto kCondition = kGraphConditionalInput;
        constexpr auto kOutput = kGraphInvalidOutput;
        using DependencyMatrix = std::vector<std::vector<bool>>;

        bool overlap(const VGraphRange& a, const VGraphRange& b) noexcept
        {
            if (a.index() != b.index())
            {
                return false;
            }
            if (const auto* x = std::get_if<BufferRange>(&a))
            {
                const auto& y = std::get<BufferRange>(b);
                return x->byte_offset < y.byte_offset + y.byte_count && y.byte_offset < x->byte_offset + x->byte_count;
            }
            const auto& x = std::get<ImageRange>(a);
            const auto& y = std::get<ImageRange>(b);
            return (static_cast<unsigned>(x.aspect) & static_cast<unsigned>(y.aspect)) != 0 &&
                   x.base_mip < y.base_mip + y.mip_count && y.base_mip < x.base_mip + x.mip_count &&
                   x.base_layer < y.base_layer + y.layer_count && y.base_layer < x.base_layer + x.layer_count;
        }

        bool normalize(const GraphResource& resource, VGraphRange& range) noexcept
        {
            if (std::holds_alternative<WholeResource>(range))
            {
                range = resource.kind() == EGraphResourceKind::BUFFER
                            ? VGraphRange{BufferRange{0, resource.buffer().byte_size}}
                            : VGraphRange{ImageRange{
                                  static_cast<EAspect>(rdesc::textureAspectMask(resource.texture().format)),
                                  0,
                                  resource.texture().mip_count,
                                  0,
                                  resource.texture().array_layers
                              }};
            }
            if (resource.kind() == EGraphResourceKind::BUFFER)
            {
                auto* x = std::get_if<BufferRange>(&range);
                const bool is_invalid_buffer_start = !x || x->byte_offset > resource.buffer().byte_size;
                if (is_invalid_buffer_start)
                {
                    return false;
                }
                if (x->byte_count == kRemainingBytes)
                {
                    x->byte_count = resource.buffer().byte_size - x->byte_offset;
                }
                return x->byte_count > 0 && x->byte_count <= resource.buffer().byte_size - x->byte_offset;
            }
            auto* x = std::get_if<ImageRange>(&range);
            const auto& d = resource.texture();
            const bool is_invalid_image_start = !x || x->base_mip > d.mip_count || x->base_layer > d.array_layers;
            if (is_invalid_image_start)
            {
                return false;
            }
            if (x->mip_count == kRemainingSubresources)
            {
                x->mip_count = d.mip_count - x->base_mip;
            }
            if (x->layer_count == kRemainingSubresources)
            {
                x->layer_count = d.array_layers - x->base_layer;
            }
            const auto mask = static_cast<unsigned>(x->aspect);
            return mask != 0 && (mask & ~rdesc::textureAspectMask(d.format)) == 0 && x->mip_count > 0 &&
                   x->layer_count > 0 && x->mip_count <= d.mip_count - x->base_mip &&
                   x->layer_count <= d.array_layers - x->base_layer;
        }

        void closure(DependencyMatrix& reachable) noexcept
        {
            for (std::size_t k = 0; k < reachable.size(); ++k)
            {
                for (std::size_t i = 0; i < reachable.size(); ++i)
                {
                    for (std::size_t j = 0; j < reachable.size(); ++j)
                    {
                        reachable[i][j] = reachable[i][j] || (reachable[i][k] && reachable[k][j]);
                    }
                }
            }
        }

        std::string quote(std::string_view value)
        {
            std::string result = "\"";
            for (const unsigned char c : value)
            {
                if (c == '"' || c == '\\')
                {
                    result += '\\';
                    result += static_cast<char>(c);
                }
                else if (c < 32)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[c >> 4];
                    result += hex[c & 15];
                }
                else
                {
                    result += static_cast<char>(c);
                }
            }
            return result + '"';
        }

        struct Analysis
        {
            LogicalGraphPlanData data;
            GraphCompileDiagnostic diagnostic;
        };

        Analysis analyze(const RenderGraphDefinition& definition, const LogicalCompileOptions& options) noexcept
        {
            Analysis result;
            auto& data = result.data;
            data.identity = logicalIdentity(definition, options);
            data.cache_identity = data.identity;
            auto& graph = data.identity;
            const auto count = graph.passes.size();
            DependencyMatrix edges(count, std::vector<bool>(count));
            DependencyMatrix essential(count, std::vector<bool>(count));
            std::vector<bool> exported_versions;
            std::vector<std::vector<std::pair<std::size_t, std::size_t>>> alternatives(count);
            auto fail = [&](error::ErrorId code, std::uint64_t pass, std::uint64_t resource)
            { result.diagnostic.error = RenderError{code, {pass, resource}}; };
            auto indexOf = [&](PassKey key) -> std::optional<std::size_t>
            {
                for (std::size_t p = 0; p < count; ++p)
                {
                    if (graph.passes[p].key == key)
                    {
                        return p;
                    }
                }
                return {};
            };
            auto edge =
                [&](std::size_t a, std::size_t b, GraphResourceId resource, const VGraphRange& range, EGraphHazard kind)
            {
                if (a != b)
                {
                    edges[a][b] = true;
                    if (kind == EGraphHazard::RAW || kind == EGraphHazard::FALLBACK)
                    {
                        essential[a][b] = true;
                    }
                    data.hazards.push_back(
                        {GraphPassId{static_cast<std::uint32_t>(a + 1)},
                         GraphPassId{static_cast<std::uint32_t>(b + 1)},
                         resource,
                         range,
                         kind}
                    );
                }
            };
            auto cycle = [&]()
            {
                std::vector<unsigned> color(count);
                std::vector<std::size_t> path;
                auto visit = [&](auto&& self, std::size_t p) -> bool
                {
                    color[p] = 1;
                    path.push_back(p);
                    for (std::size_t q = 0; q < count; ++q)
                    {
                        if (edges[p][q])
                        {
                            if (color[q] == 1)
                            {
                                auto begin = std::find(path.begin(), path.end(), q);
                                for (auto it = begin; it != path.end(); ++it)
                                {
                                    result.diagnostic.cycle_path.push_back(graph.passes[*it].key);
                                    result.diagnostic.cycle_names.push_back(graph.passes[*it].canonical_name);
                                }
                                result.diagnostic.cycle_path.push_back(graph.passes[q].key);
                                result.diagnostic.cycle_names.push_back(graph.passes[q].canonical_name);
                                fail(kGraphCycle, p + 1, q + 1);
                                return true;
                            }
                            if (color[q] == 0 && self(self, q))
                            {
                                return true;
                            }
                        }
                    }
                    path.pop_back();
                    color[p] = 2;
                    return false;
                };
                for (std::size_t p = 0; p < count; ++p)
                {
                    if (!color[p] && visit(visit, p))
                    {
                        return true;
                    }
                }
                return false;
            };
            for (const auto& dependency : graph.dependencies)
            {
                edge(
                    dependency.before.value() - 1,
                    dependency.after.value() - 1,
                    {},
                    WholeResource{},
                    EGraphHazard::ORDER
                );
            }
            for (std::size_t i = 0; i < graph.providers.size(); ++i)
            {
                const auto& provider = graph.providers[i];
                const bool invalid = !provider.semantic.isValid() || !provider.resource.isValid() ||
                                     provider.resource.value() > graph.resources.size() || !indexOf(provider.pass);
                if (invalid)
                {
                    fail(kGraphInvalidUse, provider.pass.value(), provider.resource.value());
                    return result;
                }
                for (std::size_t j = 0; j < i; ++j)
                {
                    if (graph.providers[j].semantic == provider.semantic)
                    {
                        fail(kAmbiguous, provider.pass.value(), provider.resource.value());
                        return result;
                    }
                }
            }
            for (std::size_t i = 0; i < graph.outputs.size(); ++i)
            {
                auto& output = graph.outputs[i];
                const bool invalid = !output.resource.isValid() || output.resource.value() > graph.resources.size();
                if (invalid)
                {
                    fail(kOutput, 0, output.resource.value());
                    return result;
                }
                if (const auto* semantic = std::get_if<SemanticProducer>(&output.producer))
                {
                    const auto provider = std::find_if(
                        graph.providers.begin(),
                        graph.providers.end(),
                        [&](const auto& value) { return value.semantic == semantic->semantic; }
                    );
                    if (provider == graph.providers.end() ||
                        graph.resources[provider->resource.value() - 1].description !=
                            graph.resources[output.resource.value() - 1].description)
                    {
                        fail(kOutput, 0, output.resource.value());
                        return result;
                    }
                    output.resource = provider->resource;
                    output.producer = PassProducer{provider->pass};
                }
                for (std::size_t j = 0; j < i; ++j)
                {
                    if (output.semantic.isValid() && output.semantic == graph.outputs[j].semantic)
                    {
                        fail(kOutput, 0, output.resource.value());
                        return result;
                    }
                }
            }
            for (auto& resource : graph.resources)
            {
                if (resource.origin == EGraphResourceOrigin::IMPORTED)
                {
                    if (!resource.import_contract)
                    {
                        fail(kGraphInvalidImport, 0, resource.semantic.value());
                        return result;
                    }
                    for (auto& range : resource.import_contract->initialized_ranges)
                    {
                        if (!normalize(resource, range))
                        {
                            fail(kGraphInvalidImport, 0, resource.semantic.value());
                            return result;
                        }
                    }
                }
                else if (resource.import_contract)
                {
                    fail(kGraphInvalidImport, 0, resource.semantic.value());
                    return result;
                }
            }
            if (cycle())
            {
                return result;
            }
            for (std::size_t p = 0; p < count; ++p)
            {
                auto& pass = graph.passes[p];
                data.requires_invocation_data = data.requires_invocation_data || pass.scalar_size != 0 ||
                                                !pass.bindings.empty() || pass.condition.isValid() ||
                                                pass.invocation_inputs != 0;
                // Materialize the alternative read as a proven dependency; invocation selects one binding.
                const auto declared_count = pass.uses.size();
                for (std::size_t u = 0; u < declared_count; ++u)
                {
                    if (pass.uses[u].fallback)
                    {
                        auto fallback = pass.uses[u];
                        if (fallback.access != EGraphAccess::READ)
                        {
                            fail(kCondition, p + 1, fallback.resource.value());
                            return result;
                        }
                        const auto id = fallback.fallback->resource;
                        const bool invalid = !id.isValid() || id.value() > graph.resources.size() ||
                                             graph.resources[id.value() - 1].description !=
                                                 graph.resources[fallback.resource.value() - 1].description;
                        if (invalid)
                        {
                            fail(kCondition, p + 1, id.value());
                            return result;
                        }
                        fallback.resource = id;
                        fallback.producer = fallback.fallback->producer;
                        fallback.fallback.reset();
                        fallback.field_index = ~0u;
                        alternatives[p].emplace_back(pass.uses.size(), u);
                        pass.uses.push_back(std::move(fallback));
                    }
                }
                const bool has_view_dependent_scene_inputs =
                    pass.scope == EExecutionScope::SCENE && pass.invocation_inputs != 0;
                if (has_view_dependent_scene_inputs)
                {
                    fail(kScope, p + 1, 0);
                    return result;
                }
                for (auto& use : pass.uses)
                {
                    if (auto* semantic = std::get_if<SemanticProducer>(&use.producer))
                    {
                        const GraphProvider* selected = nullptr;
                        for (const auto& provider : graph.providers)
                        {
                            if (provider.semantic == semantic->semantic)
                            {
                                if (selected)
                                {
                                    fail(kAmbiguous, p + 1, use.resource.value());
                                    return result;
                                }
                                selected = &provider;
                            }
                        }
                        if (!selected)
                        {
                            if (!use.fallback)
                            {
                                fail(kGraphMissingProducer, p + 1, use.resource.value());
                                return result;
                            }
                            use.resource = use.fallback->resource;
                            use.producer = use.fallback->producer;
                            use.fallback.reset();
                        }
                        else
                        {
                            if (!selected->resource.isValid() || selected->resource.value() > graph.resources.size() ||
                                graph.resources[selected->resource.value() - 1].description !=
                                    graph.resources[use.resource.value() - 1].description)
                            {
                                fail(kGraphInvalidUse, p + 1, use.resource.value());
                                return result;
                            }
                            use.resource = selected->resource;
                            use.producer = PassProducer{selected->pass};
                        }
                    }
                    if (use.field_index < pass.bindings.size())
                    {
                        std::visit(
                            [&](auto& binding)
                            {
                                if constexpr (requires { binding.resource; })
                                {
                                    binding.resource = use.resource;
                                }
                            },
                            pass.bindings[use.field_index].value
                        );
                    }
                    if (!use.resource.isValid() || use.resource.value() > graph.resources.size() ||
                        !normalize(graph.resources[use.resource.value() - 1], use.range))
                    {
                        fail(kGraphInvalidUse, p + 1, use.resource.value());
                        return result;
                    }
                    const auto& resource = graph.resources[use.resource.value() - 1];
                    const bool view_input = resource.persistent_scope == EPersistentScope::VIEW ||
                                            (resource.import_contract && resource.import_contract->temporal_history) ||
                                            (resource.kind() == EGraphResourceKind::IMAGE &&
                                             resource.texture().extent_kind != EExtentKind::ABSOLUTE);
                    const bool target_input = resource.target_semantic.isValid();
                    if ((pass.scope == EExecutionScope::SCENE && (view_input || target_input)) ||
                        (pass.scope == EExecutionScope::VIEW && target_input))
                    {
                        fail(kScope, p + 1, use.resource.value());
                        return result;
                    }
                    const bool valid_local_pair =
                        !use.local_read || std::any_of(
                                               pass.uses.begin(),
                                               pass.uses.end(),
                                               [&](const auto& other)
                                               {
                                                   return !other.local_read && other.resource == use.resource &&
                                                          other.range == use.range &&
                                                          other.access != EGraphAccess::READ &&
                                                          (other.usage == EGraphUsage::COLOR_ATTACHMENT ||
                                                           other.usage == EGraphUsage::DEPTH_ATTACHMENT);
                                               }
                                           );
                    if (use.local_read && (!options.allow_local_read || pass.kind != EPassKind::GRAPHICS ||
                                           use.usage != EGraphUsage::INPUT_ATTACHMENT || !valid_local_pair))
                    {
                        fail(kGraphInvalidUse, p + 1, use.resource.value());
                        return result;
                    }
                    if (const auto* source = std::get_if<PassProducer>(&use.producer))
                    {
                        const auto producer = indexOf(source->pass);
                        const bool is_invalid_producer_reference = !producer || (*producer == p && !use.local_read);
                        if (is_invalid_producer_reference)
                        {
                            fail(kGraphMissingProducer, p + 1, use.resource.value());
                            return result;
                        }
                        edge(*producer, p, use.resource, use.range, EGraphHazard::RAW);
                    }
                }
            }
            if (cycle())
            {
                return result;
            }
            auto reach = edges;
            closure(reach);
            data.live.assign(count, !options.cull_unused);
            data.scene_share_eligible.assign(count, false);
            data.lifetimes.resize(graph.resources.size());
            for (std::size_t r = 0; r < graph.resources.size(); ++r)
            {
                const GraphResourceId id{static_cast<std::uint32_t>(r + 1)};
                const auto& resource = graph.resources[r];
                const bool image = resource.kind() == EGraphResourceKind::IMAGE;
                if (resource.origin == EGraphResourceOrigin::IMPORTED)
                {
                    data.imports.push_back(id);
                }
                std::vector<std::uint64_t> x{0}, y{0};
                if (image)
                {
                    x.push_back(resource.texture().mip_count);
                    y.push_back(resource.texture().array_layers);
                }
                else
                {
                    x.push_back(resource.buffer().byte_size);
                }
                auto endpoints = [&](const VGraphRange& range)
                {
                    if (image)
                    {
                        const auto& a = std::get<ImageRange>(range);
                        x.push_back(a.base_mip);
                        x.push_back(a.base_mip + a.mip_count);
                        y.push_back(a.base_layer);
                        y.push_back(a.base_layer + a.layer_count);
                    }
                    else
                    {
                        const auto& a = std::get<BufferRange>(range);
                        x.push_back(a.byte_offset);
                        x.push_back(a.byte_offset + a.byte_count);
                    }
                };
                for (const auto& pass : graph.passes)
                {
                    for (const auto& use : pass.uses)
                    {
                        if (use.resource == id)
                        {
                            endpoints(use.range);
                        }
                    }
                }
                if (resource.import_contract)
                {
                    for (const auto& range : resource.import_contract->initialized_ranges)
                    {
                        endpoints(range);
                    }
                }
                for (auto& output : graph.outputs)
                {
                    if (output.resource == id)
                    {
                        if (!normalize(resource, output.range))
                        {
                            fail(kOutput, 0, id.value());
                            return result;
                        }
                        endpoints(output.range);
                    }
                }
                for (auto* points : {&x, &y})
                {
                    std::sort(points->begin(), points->end());
                    points->erase(std::unique(points->begin(), points->end()), points->end());
                }
                if (!image)
                {
                    y.push_back(1);
                }
                auto compileCell = [&](const VGraphRange& cell) -> bool
                {
                    std::vector<std::size_t> writers;
                    for (std::size_t p = 0; p < count; ++p)
                    {
                        for (const auto& use : graph.passes[p].uses)
                        {
                            if (use.resource == id && overlap(cell, use.range) && use.access != EGraphAccess::READ)
                            {
                                writers.push_back(p);
                                break;
                            }
                        }
                    }
                    for (std::size_t a = 0; a < writers.size(); ++a)
                    {
                        for (std::size_t b = a + 1; b < writers.size(); ++b)
                        {
                            if (!reach[writers[a]][writers[b]] && !reach[writers[b]][writers[a]])
                            {
                                fail(kAmbiguous, writers[a] + 1, id.value());
                                return false;
                            }
                        }
                    }
                    std::sort(writers.begin(), writers.end(), [&](auto a, auto b) { return reach[a][b]; });
                    const auto first_version = static_cast<std::uint32_t>(data.versions.size());
                    const bool imported =
                        resource.import_contract && std::any_of(
                                                        resource.import_contract->initialized_ranges.begin(),
                                                        resource.import_contract->initialized_ranges.end(),
                                                        [&](const auto& range) { return overlap(cell, range); }
                                                    );
                    if (imported)
                    {
                        data.versions.push_back({id, cell, {}, {}, {}});
                        exported_versions.push_back(false);
                    }
                    for (auto p : writers)
                    {
                        data.versions.push_back({id, cell, GraphPassId{static_cast<std::uint32_t>(p + 1)}, {}, {}});
                        exported_versions.push_back(false);
                    }
                    for (std::size_t w = 1; w < writers.size(); ++w)
                    {
                        edge(writers[w - 1], writers[w], id, cell, EGraphHazard::WAW);
                    }
                    auto select = [&](const VGraphProducer& source, std::optional<std::size_t> reader) -> int
                    {
                        if (std::holds_alternative<ImportedProducer>(source))
                        {
                            return imported ? -1 : -2;
                        }
                        if (const auto* p = std::get_if<PassProducer>(&source))
                        {
                            for (std::size_t w = 0; w < writers.size(); ++w)
                            {
                                if (graph.passes[writers[w]].key == p->pass)
                                {
                                    return static_cast<int>(w);
                                }
                            }
                            return -2;
                        }
                        if (!std::holds_alternative<AutomaticProducer>(source))
                        {
                            return -2;
                        }
                        if (!reader)
                        {
                            return writers.empty() ? (imported ? -1 : -2) : static_cast<int>(writers.size() - 1);
                        }
                        int selected = imported ? -1 : -2;
                        for (std::size_t w = 0; w < writers.size(); ++w)
                        {
                            const auto writer = writers[w];
                            if (writer == *reader || reach[*reader][writer])
                            {
                                continue;
                            }
                            if (reach[writer][*reader] || writers.size() == 1)
                            {
                                selected = static_cast<int>(w);
                            }
                            else
                            {
                                return -3;
                            }
                        }
                        return selected;
                    };
                    for (std::size_t p = 0; p < count; ++p)
                    {
                        for (const auto& use : graph.passes[p].uses)
                        {
                            if (use.resource != id || !overlap(cell, use.range) || use.access == EGraphAccess::WRITE)
                            {
                                continue;
                            }
                            int selected = select(use.producer, p);
                            if (use.local_read)
                            {
                                for (std::size_t w = 0; w < writers.size(); ++w)
                                {
                                    if (writers[w] == p)
                                    {
                                        selected = static_cast<int>(w);
                                    }
                                }
                            }
                            if (selected < -1)
                            {
                                fail(selected == -3 ? kAmbiguous : kGraphMissingProducer, p + 1, id.value());
                                return false;
                            }
                            auto& version =
                                data.versions[first_version + static_cast<unsigned>(selected + (imported ? 1 : 0))];
                            version.readers.push_back(GraphPassId{static_cast<std::uint32_t>(p + 1)});
                            if (selected >= 0)
                            {
                                const auto writer = writers[selected];
                                const auto& source = graph.passes[writer];
                                if (writer != p)
                                {
                                    for (const auto& field : source.bindings)
                                    {
                                        if (const auto* attachment = std::get_if<AttachmentBinding>(&field.value))
                                        {
                                            if (attachment->resource == id &&
                                                overlap(cell, VGraphRange{attachment->range}))
                                            {
                                                const bool stencil =
                                                    image && std::get<ImageRange>(cell).aspect == EAspect::STENCIL;
                                                const auto store =
                                                    stencil ? attachment->stencil_store : attachment->store;
                                                if (store == EStoreOp::DISCARD)
                                                {
                                                    fail(kGraphMissingProducer, p + 1, id.value());
                                                    return false;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (source.condition.isValid() && source.condition != graph.passes[p].condition)
                                {
                                    if (!use.fallback)
                                    {
                                        fail(kCondition, p + 1, id.value());
                                        return false;
                                    }
                                    const auto fallback = use.fallback->resource;
                                    const auto use_index =
                                        static_cast<std::uint32_t>(&use - graph.passes[p].uses.data());
                                    const GraphPassId consumer{static_cast<std::uint32_t>(p + 1)};
                                    const GraphPassId condition_writer{static_cast<std::uint32_t>(writer + 1)};
                                    const auto previous = std::find_if(
                                        data.input_choices.begin(),
                                        data.input_choices.end(),
                                        [&](const auto& choice)
                                        { return choice.consumer == consumer && choice.use_index == use_index; }
                                    );
                                    if (previous != data.input_choices.end() &&
                                        graph.passes[previous->conditional_producer.value() - 1].condition !=
                                            source.condition)
                                    {
                                        fail(kCondition, p + 1, id.value());
                                        return false;
                                    }
                                    if (previous == data.input_choices.end())
                                    {
                                        data.input_choices.push_back({consumer, use_index, condition_writer, fallback});
                                    }
                                }
                                if (static_cast<unsigned>(graph.passes[p].scope) < static_cast<unsigned>(source.scope))
                                {
                                    fail(kScope, p + 1, id.value());
                                    return false;
                                }
                                edge(writer, p, id, cell, EGraphHazard::RAW);
                            }
                            const auto next = static_cast<std::size_t>(selected + 1);
                            bool mutually_exclusive = false;
                            if (next < writers.size() && graph.passes[writers[next]].condition.isValid())
                            {
                                for (const auto& [alternate, primary] : alternatives[p])
                                {
                                    if (alternate == static_cast<std::size_t>(&use - graph.passes[p].uses.data()))
                                    {
                                        const auto& primary_use = graph.passes[p].uses[primary];
                                        if (const auto* source = std::get_if<PassProducer>(&primary_use.producer))
                                        {
                                            const auto producer = indexOf(source->pass);
                                            mutually_exclusive = producer && graph.passes[*producer].condition ==
                                                                                 graph.passes[writers[next]].condition;
                                        }
                                    }
                                }
                            }
                            if (next < writers.size() && writers[next] != p && !mutually_exclusive)
                            {
                                edge(p, writers[next], id, cell, EGraphHazard::WAR);
                            }
                        }
                    }
                    for (const auto& output : graph.outputs)
                    {
                        if (output.resource == id && overlap(cell, output.range))
                        {
                            const int selected = select(output.producer, {});
                            if (selected < -1)
                            {
                                fail(kOutput, 0, id.value());
                                return false;
                            }
                            if (output.kind == EGraphOutput::PRESENT && (!image || !resource.target_semantic.isValid()))
                            {
                                fail(kOutput, 0, id.value());
                                return false;
                            }
                            if (output.kind == EGraphOutput::EXTERNAL_WRITE &&
                                resource.origin != EGraphResourceOrigin::IMPORTED)
                            {
                                fail(kOutput, 0, id.value());
                                return false;
                            }
                            if (selected >= 0 && static_cast<std::size_t>(selected + 1) != writers.size())
                            {
                                fail(kOutput, 0, id.value());
                                return false;
                            }
                            if (output.kind > EGraphOutput::EXTERNAL_WRITE ||
                                (output.kind == EGraphOutput::EXTERNAL_WRITE && selected < 0))
                            {
                                fail(kOutput, 0, id.value());
                                return false;
                            }
                            exported_versions[first_version + static_cast<unsigned>(selected + (imported ? 1 : 0))] =
                                true;
                            if (selected >= 0)
                            {
                                for (const auto& field : graph.passes[writers[selected]].bindings)
                                {
                                    if (const auto* attachment = std::get_if<AttachmentBinding>(&field.value))
                                    {
                                        if (attachment->resource == id && overlap(cell, VGraphRange{attachment->range}))
                                        {
                                            const bool stencil =
                                                image && std::get<ImageRange>(cell).aspect == EAspect::STENCIL;
                                            const auto store = stencil ? attachment->stencil_store : attachment->store;
                                            if (store == EStoreOp::DISCARD)
                                            {
                                                fail(kOutput, 0, id.value());
                                                return false;
                                            }
                                        }
                                    }
                                }

                                if (graph.passes[writers[selected]].condition.isValid())
                                {
                                    fail(kCondition, writers[selected] + 1, id.value());
                                    return false;
                                }
                                data.live[writers[selected]] = true;
                            }
                        }
                    }
                    return true;
                };
                const auto aspects = image ? rdesc::textureAspectMask(resource.texture().format) : 1u;
                for (unsigned aspect : {1u, 2u, 4u})
                {
                    if ((aspects & aspect) != 0)
                    {
                        for (std::size_t ix = 1; ix < x.size(); ++ix)
                        {
                            for (std::size_t iy = 1; iy < y.size(); ++iy)
                            {
                                VGraphRange cell = image ? VGraphRange{ImageRange{
                                                               static_cast<EAspect>(aspect),
                                                               static_cast<std::uint32_t>(x[ix - 1]),
                                                               static_cast<std::uint32_t>(x[ix] - x[ix - 1]),
                                                               static_cast<std::uint32_t>(y[iy - 1]),
                                                               static_cast<std::uint32_t>(y[iy] - y[iy - 1])
                                                           }}
                                                         : VGraphRange{BufferRange{x[ix - 1], x[ix] - x[ix - 1]}};
                                if (!compileCell(cell))
                                {
                                    return result;
                                }
                            }
                        }
                    }
                }
            }
            for (const auto& output : graph.outputs)
            {
                const bool is_invalid_output_resource =
                    !output.resource.isValid() || output.resource.value() > graph.resources.size();
                if (is_invalid_output_resource)
                {
                    fail(kOutput, 0, output.resource.value());
                    return result;
                }
            }
            if (cycle())
            {
                return result;
            }
            // Host readback is a typed external effect, not an arbitrary keep-alive flag.
            for (std::size_t p = 0; p < count; ++p)
            {
                if (graph.passes[p].kind == EPassKind::HOST_READBACK)
                {
                    const auto& pass = graph.passes[p];
                    const bool valid =
                        !pass.uses.empty() && !pass.condition.isValid() &&
                        std::all_of(
                            pass.uses.begin(),
                            pass.uses.end(),
                            [](const auto& use)
                            { return use.access == EGraphAccess::READ && use.usage == EGraphUsage::TRANSFER; }
                        );
                    if (!valid)
                    {
                        fail(kOutput, p + 1, 0);
                        return result;
                    }
                    data.live[p] = true;
                }
            }
            // Read/producer and explicit order roots retain inputs. Anti-dependencies do not create liveness.
            for (std::size_t round = 0; round < count; ++round)
            {
                for (std::size_t a = 0; a < count; ++a)
                {
                    for (std::size_t b = 0; b < count; ++b)
                    {
                        if (essential[a][b] && data.live[b])
                        {
                            data.live[a] = true;
                        }
                    }
                }
            }
            std::vector<unsigned> indegree(count);
            for (std::size_t a = 0; a < count; ++a)
            {
                for (std::size_t b = 0; b < count; ++b)
                {
                    if (edges[a][b])
                    {
                        data.dependencies.push_back(
                            {GraphPassId{static_cast<std::uint32_t>(a + 1)},
                             GraphPassId{static_cast<std::uint32_t>(b + 1)}}
                        );
                        if (data.live[a] && data.live[b])
                        {
                            ++indegree[b];
                        }
                    }
                }
            }
            std::vector<bool> emitted(count);
            for (;;)
            {
                std::optional<std::size_t> chosen;
                for (std::size_t p = 0; p < count; ++p)
                {
                    if (data.live[p] && !emitted[p] && indegree[p] == 0)
                    {
                        if (!chosen || graph.passes[p].canonical_name < graph.passes[*chosen].canonical_name)
                        {
                            chosen = p;
                        }
                    }
                }
                if (!chosen)
                {
                    break;
                }
                const auto p = *chosen;
                emitted[p] = true;
                data.order.push_back(GraphPassId{static_cast<std::uint32_t>(p + 1)});
                for (std::size_t q = 0; q < count; ++q)
                {
                    if (edges[p][q] && data.live[q])
                    {
                        --indegree[q];
                    }
                }
                data.scene_share_eligible[p] = graph.passes[p].scope == EExecutionScope::SCENE;
            }
            std::vector<std::optional<std::uint32_t>> positions(count);
            for (std::size_t i = 0; i < data.order.size(); ++i)
            {
                positions[data.order[i].value() - 1] = static_cast<std::uint32_t>(i);
            }
            for (auto& version : data.versions)
            {
                auto include = [&](GraphPassId pass)
                {
                    const bool has_no_live_position = !pass.isValid() || !positions[pass.value() - 1];
                    if (has_no_live_position)
                    {
                        return;
                    }
                    const auto p = *positions[pass.value() - 1];
                    if (!version.lifetime)
                    {
                        version.lifetime = GraphResourceLifetime{p, p};
                    }
                    else
                    {
                        version.lifetime->first_pass = std::min(version.lifetime->first_pass, p);
                        version.lifetime->last_pass = std::max(version.lifetime->last_pass, p);
                    }
                };
                include(version.writer);
                for (auto reader : version.readers)
                {
                    include(reader);
                }
                const auto version_index = static_cast<std::size_t>(&version - data.versions.data());
                if (exported_versions[version_index])
                {
                    const auto terminal = static_cast<std::uint32_t>(data.order.size());
                    if (!version.lifetime)
                    {
                        version.lifetime = GraphResourceLifetime{0, terminal};
                    }
                    else
                    {
                        version.lifetime->last_pass = terminal;
                    }
                }
                if (version.lifetime)
                {
                    auto& lifetime = data.lifetimes[version.resource.value() - 1];
                    if (!lifetime)
                    {
                        lifetime = version.lifetime;
                    }
                    else
                    {
                        lifetime->first_pass = std::min(lifetime->first_pass, version.lifetime->first_pass);
                        lifetime->last_pass = std::max(lifetime->last_pass, version.lifetime->last_pass);
                    }
                }
            }
            for (std::size_t a = 0; a < data.versions.size(); ++a)
            {
                for (std::size_t b = a + 1; b < data.versions.size(); ++b)
                {
                    const auto& x = data.versions[a];
                    const auto& y = data.versions[b];
                    const bool is_unavailable_reuse_pair = x.resource == y.resource || !x.lifetime || !y.lifetime;
                    if (is_unavailable_reuse_pair)
                    {
                        continue;
                    }
                    const auto& xr = graph.resources[x.resource.value() - 1];
                    const auto& yr = graph.resources[y.resource.value() - 1];
                    if (xr.origin != EGraphResourceOrigin::TRANSIENT || yr.origin != EGraphResourceOrigin::TRANSIENT ||
                        xr.description != yr.description)
                    {
                        continue;
                    }
                    if (x.lifetime->last_pass < y.lifetime->first_pass)
                    {
                        data.reuse_candidates.push_back({static_cast<unsigned>(a), static_cast<unsigned>(b)});
                    }
                    else if (y.lifetime->last_pass < x.lifetime->first_pass)
                    {
                        data.reuse_candidates.push_back({static_cast<unsigned>(b), static_cast<unsigned>(a)});
                    }
                }
            }
            auto inherited = essential;
            closure(inherited);
            data.scene_sources.resize(count);
            for (std::size_t p = 0; p < count; ++p)
            {
                if (data.scene_share_eligible[p])
                {
                    for (std::size_t q = 0; q < count; ++q)
                    {
                        if (q == p || inherited[q][p])
                        {
                            if (graph.passes[q].scope != EExecutionScope::SCENE)
                            {
                                fail(kScope, p + 1, q + 1);
                                return result;
                            }
                            data.scene_sources[p].push_back(GraphPassId{static_cast<std::uint32_t>(q + 1)});
                        }
                    }
                }
            }
            auto rangeJson = [&](const VGraphRange& range)
            {
                std::ostringstream value;
                if (const auto* buffer = std::get_if<BufferRange>(&range))
                {
                    value << "{\"byte_offset\":" << buffer->byte_offset << ",\"byte_count\":" << buffer->byte_count
                          << '}';
                }
                else if (const auto* image = std::get_if<ImageRange>(&range))
                {
                    value << "{\"aspect\":" << static_cast<unsigned>(image->aspect) << ",\"mip\":" << image->base_mip
                          << ",\"mips\":" << image->mip_count << ",\"layer\":" << image->base_layer
                          << ",\"layers\":" << image->layer_count << '}';
                }
                else
                {
                    value << "null";
                }
                return value.str();
            };
            std::ostringstream json;
            json << "{\"passes\":[";
            for (std::size_t p = 0; p < count; ++p)
            {
                if (p)
                {
                    json << ',';
                }
                const auto& pass = graph.passes[p];
                json << "{\"name\":" << quote(pass.canonical_name) << ",\"key\":" << pass.key.value()
                     << ",\"shader\":" << quote(pass.shader_name) << ",\"scope\":" << static_cast<unsigned>(pass.scope)
                     << ",\"condition\":" << pass.condition.value() << ",\"live\":" << (data.live[p] ? "true" : "false")
                     << ",\"scene_share_eligible\":" << (data.scene_share_eligible[p] ? "true" : "false")
                     << ",\"reason\":"
                     << quote(
                            data.live[p] ? "reachable output/producer or explicit retention option"
                                         : "unreachable from output"
                        )
                     << '}';
            }
            json << "],\"order\":[";
            for (std::size_t i = 0; i < data.order.size(); ++i)
            {
                if (i)
                {
                    json << ',';
                }
                json << data.order[i].value();
            }
            json << "],\"hazards\":[";
            for (std::size_t i = 0; i < data.hazards.size(); ++i)
            {
                if (i)
                {
                    json << ',';
                }
                const auto& hazard = data.hazards[i];
                json << "{\"before\":" << hazard.before.value() << ",\"after\":" << hazard.after.value()
                     << ",\"kind\":" << static_cast<unsigned>(hazard.kind)
                     << ",\"resource\":" << hazard.resource.value() << ",\"range\":" << rangeJson(hazard.range) << '}';
            }
            json << "],\"versions\":[";
            for (std::size_t i = 0; i < data.versions.size(); ++i)
            {
                if (i)
                {
                    json << ',';
                }
                const auto& version = data.versions[i];
                json << "{\"resource\":" << version.resource.value() << ",\"writer\":" << version.writer.value()
                     << ",\"range\":" << rangeJson(version.range) << ",\"readers\":[";
                for (std::size_t j = 0; j < version.readers.size(); ++j)
                {
                    if (j)
                    {
                        json << ',';
                    }
                    json << version.readers[j].value();
                }
                json << "],\"lifetime\":";
                if (version.lifetime)
                {
                    json << '[' << version.lifetime->first_pass << ',' << version.lifetime->last_pass << ']';
                }
                else
                {
                    json << "null";
                }
                json << '}';
            }
            json << "],\"imports\":[";
            for (std::size_t i = 0; i < data.imports.size(); ++i)
            {
                if (i)
                {
                    json << ',';
                }
                json << data.imports[i].value();
            }
            json << "],\"exports\":[";
            for (std::size_t i = 0; i < graph.outputs.size(); ++i)
            {
                if (i)
                {
                    json << ',';
                }
                const auto& output = graph.outputs[i];
                json << "{\"resource\":" << output.resource.value()
                     << ",\"kind\":" << static_cast<unsigned>(output.kind) << ",\"range\":" << rangeJson(output.range)
                     << '}';
            }
            json << "],\"cache_identity\":{\"equality\":\"exact typed fields\","
                    "\"includes\":[\"resources and import initialization\",\"pass identity and shader interface\","
                    "\"uses and producer versions\",\"bindings structural operations\",\"conditions and scope\","
                    "\"providers outputs explicit order compile options\"],"
                    "\"excludes\":[\"scalar bytes\",\"sampler values\",\"clear values\",\"camera\","
                    "\"time serial revisions backing dynamic offsets\"]},\"native_alias_proven\":false}";
            data.diagnostics_json = json.str();
            result.diagnostic.json = data.diagnostics_json;
            return result;
        }
    } // namespace

    LogicalGraphIdentity logicalIdentity(
        const RenderGraphDefinition& definition,
        const LogicalCompileOptions& options
    ) noexcept
    {
        LogicalGraphIdentity result;
        result.options = options;
        result.resources.assign(definition.resources().begin(), definition.resources().end());
        result.dependencies.assign(definition.dependencies().begin(), definition.dependencies().end());
        result.outputs.assign(definition.outputs().begin(), definition.outputs().end());
        result.providers.assign(definition.providers().begin(), definition.providers().end());
        for (auto& resource : result.resources)
        {
            if (resource.import_contract)
            {
                for (auto& range : resource.import_contract->initialized_ranges)
                {
                    normalize(resource, range);
                }
            }
        }
        for (auto& output : result.outputs)
        {
            if (output.resource.isValid() && output.resource.value() <= result.resources.size())
            {
                normalize(result.resources[output.resource.value() - 1], output.range);
            }
        }
        for (const auto& pass : definition.passes())
        {
            LogicalPass p{
                pass.uses,
                pass.key,
                pass.shader,
                pass.kind,
                pass.scope,
                pass.canonical_name,
                pass.shader_name,
                pass.schema_name,
                pass.shader_declarations,
                pass.scalar_fields,
                {},
                pass.initial_scalars.size(),
                pass.condition,
                pass.invocation_inputs
            };
            for (const auto& f : pass.bindings)
            {
                VGraphRange range = f.resource_kind == EGraphResourceKind::IMAGE ? VGraphRange{f.image_range}
                                                                                 : VGraphRange{f.buffer_range};
                VGraphBinding value;
                using R = rdesc::EPassFieldRole;
                if (f.role == R::SAMPLER)
                {
                    value = SamplerBinding{f.paired_texture};
                }
                else if (f.role == R::COLOR_ATTACHMENT || f.role == R::DEPTH_STENCIL || f.role == R::RESOLVE)
                {
                    value = AttachmentBinding{
                        f.resource,
                        f.image_range,
                        f.load,
                        f.stencil_load,
                        f.store,
                        f.stencil_store,
                        f.role,
                        f.paired_texture
                    };
                }
                else if (f.role == R::TRANSFER_SOURCE || f.role == R::TRANSFER_DESTINATION || f.role == R::VERTEX ||
                         f.role == R::INDEX || f.role == R::INDIRECT)
                {
                    value = TransferBinding{f.resource, range, f.role};
                }
                else
                {
                    value = ShaderResourceBinding{
                        f.resource,
                        f.role,
                        range,
                        f.dimension,
                        f.image_format,
                        f.array_count,
                        f.element_stride,
                        f.descriptor_array
                    };
                }
                p.bindings.push_back(
                    {f.path,
                     f.shader_name,
                     f.semantic,
                     f.array_element,
                     f.stages,
                     f.owner,
                     f.frequency,
                     f.required,
                     std::move(value)}
                );
            }
            result.passes.push_back(std::move(p));
        }
        return result;
    }

    std::vector<std::string> explainLogicalCompatibility(
        const LogicalGraphPlan& plan,
        const RenderGraphDefinition& candidate
    ) noexcept
    {
        const auto& a = plan.cacheIdentity();
        const auto b = logicalIdentity(candidate, a.options);
        std::vector<std::string> differences;
        auto compare = [&](const auto& x, const auto& y, std::string path)
        {
            if (x != y)
            {
                differences.push_back(std::move(path));
            }
        };
        compare(a.resources.size(), b.resources.size(), "resources.size");
        for (std::size_t i = 0; i < std::min(a.resources.size(), b.resources.size()); ++i)
        {
            const auto& x = a.resources[i];
            const auto& y = b.resources[i];
            const auto path = "resources[" + std::to_string(i) + "].";
            compare(x.description, y.description, path + "description");
            compare(x.origin, y.origin, path + "origin");
            compare(x.target_semantic, y.target_semantic, path + "target_semantic");
            compare(x.persistent_scope, y.persistent_scope, path + "persistent_scope");
            compare(x.canonical_name, y.canonical_name, path + "canonical_name");
            compare(x.semantic, y.semantic, path + "semantic");
            compare(x.import_contract, y.import_contract, path + "import_contract");
        }
        compare(a.passes.size(), b.passes.size(), "passes.size");
        for (std::size_t i = 0; i < std::min(a.passes.size(), b.passes.size()); ++i)
        {
            const auto& x = a.passes[i];
            const auto& y = b.passes[i];
            const auto path = "passes[" + std::to_string(i) + "].";
            compare(x.key, y.key, path + "key");
            compare(x.shader, y.shader, path + "shader");
            compare(x.kind, y.kind, path + "kind");
            compare(x.scope, y.scope, path + "scope");
            compare(x.canonical_name, y.canonical_name, path + "canonical_name");
            compare(x.shader_name, y.shader_name, path + "shader_name");
            compare(x.schema_name, y.schema_name, path + "schema_name");
            compare(x.shader_declarations, y.shader_declarations, path + "shader_declarations");
            compare(x.scalar_fields, y.scalar_fields, path + "scalar_fields");
            compare(x.bindings, y.bindings, path + "bindings");
            compare(x.scalar_size, y.scalar_size, path + "scalar_size");
            compare(x.condition, y.condition, path + "condition");
            compare(x.invocation_inputs, y.invocation_inputs, path + "invocation_inputs");
            compare(x.uses.size(), y.uses.size(), path + "uses.size");
            for (std::size_t u = 0; u < std::min(x.uses.size(), y.uses.size()); ++u)
            {
                const auto& left = x.uses[u];
                const auto& right = y.uses[u];
                const auto use_path = path + "uses[" + std::to_string(u) + "].";
                compare(left.resource, right.resource, use_path + "resource");
                compare(left.access, right.access, use_path + "access");
                compare(left.usage, right.usage, use_path + "usage");
                compare(left.range, right.range, use_path + "range");
                compare(left.stages, right.stages, use_path + "stages");
                compare(left.minimum_bytes, right.minimum_bytes, use_path + "minimum_bytes");
                compare(left.byte_alignment, right.byte_alignment, use_path + "byte_alignment");
                compare(left.element_stride, right.element_stride, use_path + "element_stride");
                compare(left.field_index, right.field_index, use_path + "field_index");
                compare(left.producer, right.producer, use_path + "producer");
                compare(left.fallback, right.fallback, use_path + "fallback");
                compare(left.local_read, right.local_read, use_path + "local_read");
            }
        }
        compare(a.dependencies, b.dependencies, "explicit_order");
        compare(a.outputs, b.outputs, "outputs");
        compare(a.providers, b.providers, "providers");
        return differences;
    }

    RenderResult<LogicalGraphPlan> compileLogicalGraph(
        const RenderGraphDefinition& definition,
        const LogicalCompileOptions& options
    ) noexcept
    {
        auto result = analyze(definition, options);
        if (result.diagnostic.error)
        {
            return cxx::unexpected(*result.diagnostic.error);
        }
        return LogicalGraphPlan{std::move(result.data)};
    }

    GraphCompileDiagnostic diagnoseLogicalGraph(
        const RenderGraphDefinition& definition,
        const LogicalCompileOptions& options
    ) noexcept
    {
        auto result = analyze(definition, options);
        if (result.diagnostic.error)
        {
            std::ostringstream json;
            json << "{\"error\":" << result.diagnostic.error->type << ",\"cycle\":[";
            for (std::size_t i = 0; i < result.diagnostic.cycle_names.size(); ++i)
            {
                if (i)
                {
                    json << ',';
                }
                json << quote(result.diagnostic.cycle_names[i]);
            }
            json << "]}";
            result.diagnostic.json = json.str();
        }
        return std::move(result.diagnostic);
    }
} // namespace lux::render
