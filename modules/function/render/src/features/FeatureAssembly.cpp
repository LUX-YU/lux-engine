#include <lux/engine/render/features/FeatureAssembly.hpp>

#include <algorithm>
#include <numeric>
#include <tuple>

namespace lux::render
{
    namespace
    {
        RenderError failure(std::uint64_t type, std::size_t feature = 0, std::size_t port = 0) noexcept
        {
            return {type, {feature, port}};
        }

        struct PlacedPort final
        {
            FeaturePort declaration;
            std::vector<GraphStorageBinding>* storage = nullptr;
        };

        struct Contribution final
        {
            std::vector<PlacedPort> declarations;
            std::vector<ResolvedResource> resolved;
            std::vector<std::size_t> incoming;
            std::vector<ResourceVersion> supplied;
            std::size_t first_resource = 0, resource_count = 0;
            std::size_t first_argument = 0, argument_count = 0;
            std::size_t first_binding = 0, binding_count = 0;
            bool built = false;
        };

        std::size_t portIndex(const Contribution& contribution, std::string_view key) noexcept
        {
            const auto found = std::ranges::find_if(
                contribution.declarations,
                [&](const auto& port) noexcept { return port.declaration.key == key; }
            );
            return static_cast<std::size_t>(found - contribution.declarations.begin());
        }
    } // namespace

    std::string featureKey(FeatureName type, std::string_view instance, std::string_view local) noexcept
    {
        return "feature:" + std::to_string(type.name().size()) + ":" + std::string(type.name()) + ":" +
               std::to_string(instance.size()) + ":" + std::string(instance) + ":" + std::string(local);
    }

    RenderResult<FeatureAssembly> composeFeatures(
        std::span<const FeatureBuildInput> inputs,
        std::size_t config_byte_limit,
        std::span<const FeatureConnection> connections,
        std::span<const FeatureExport> exports,
        std::span<const FeatureInput> external
    ) noexcept
    {
        FeatureAssembly result;
        std::vector<Contribution> work(inputs.size());
        std::vector<std::size_t> order(inputs.size());
        std::iota(order.begin(), order.end(), 0);
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            const bool invalid_definition = !input.descriptor || !input.graph || !input.graph->describe;
            if (invalid_definition)
            {
                return cxx::unexpected(failure(kFeatureContract, i));
            }
            const bool invalid_identity = !input.descriptor->type.isValid() || input.contribution_key.empty();
            const bool has_config = input.config && *input.config;
            const bool requires_config = !input.descriptor->config.token.name().empty();
            const bool invalid_config =
                has_config != requires_config || (has_config && (input.descriptor->config != input.config->typeInfo() ||
                                                                 input.config->retainedBytes() > config_byte_limit));
            if (invalid_identity || invalid_config)
            {
                return cxx::unexpected(failure(invalid_config ? kInvalidConfig : kFeatureContract, i));
            }
            FeaturePorts ports;
            auto described =
                input.graph->describe(has_config ? input.config->data() : nullptr, input.contribution_key, ports);
            if (!described)
            {
                return cxx::unexpected(described.error());
            }
            const auto append = [&](auto& source, std::vector<GraphStorageBinding>* storage)
            {
                for (auto& port : source)
                {
                    work[i].declarations.push_back({std::move(port), storage});
                }
            };
            append(ports.resources, nullptr);
            append(ports.storage.global, &result.external_bindings.global);
            append(ports.storage.scene, &result.external_bindings.scene);
            append(ports.storage.view, &result.external_bindings.view);
            append(ports.storage.attachment, &result.external_bindings.attachment);
            auto& declarations = work[i].declarations;
            std::ranges::sort(declarations, {}, [](const auto& port) { return port.declaration.key; });
            for (std::size_t p = 0; p < declarations.size(); ++p)
            {
                const auto& port = declarations[p];
                const auto& d = port.declaration;
                const bool duplicate = p != 0 && declarations[p - 1].declaration.key == d.key;
                const bool invalid =
                    d.key.empty() || d.direction > EFeaturePort::OPTIONAL || bool(d.storage) != bool(port.storage) ||
                    (d.storage && (d.direction != EFeaturePort::OUTPUT || d.origin != EGraphOrigin::IMPORTED));
                if (duplicate || invalid)
                {
                    return cxx::unexpected(failure(duplicate ? kFeatureDuplicate : kFeatureContract, i, p));
                }
                work[i].resolved.push_back({d.key, {}});
            }
            work[i].incoming.resize(declarations.size(), SIZE_MAX);
        }
        std::ranges::sort(
            order,
            [&](auto a, auto b)
            {
                return std::tuple{inputs[a].descriptor->type.name(), inputs[a].contribution_key} <
                       std::tuple{inputs[b].descriptor->type.name(), inputs[b].contribution_key};
            }
        );
        for (std::size_t i = 1; i < order.size(); ++i)
        {
            const auto& a = inputs[order[i - 1]];
            const auto& b = inputs[order[i]];
            const bool same_type = a.descriptor->type == b.descriptor->type;
            const bool conflicting = same_type && (a.descriptor != b.descriptor || a.graph != b.graph ||
                                                   a.contribution_key == b.contribution_key);
            if (conflicting)
            {
                return cxx::unexpected(failure(kFeatureDuplicate, order[i]));
            }
        }
        for (std::size_t c = 0; c < connections.size(); ++c)
        {
            const auto& connection = connections[c];
            const auto from = connection.source.contribution, to = connection.destination.contribution;
            if (from >= work.size() || to >= work.size())
            {
                return cxx::unexpected(failure(kFeatureContract, to, c));
            }
            const auto source = portIndex(work[from], connection.source.port);
            const auto destination = portIndex(work[to], connection.destination.port);
            const bool missing =
                source == work[from].declarations.size() || destination == work[to].declarations.size();
            if (missing)
            {
                return cxx::unexpected(failure(kFeatureContract, to, c));
            }
            const auto& a = work[from].declarations[source].declaration;
            const auto& b = work[to].declarations[destination].declaration;
            const bool invalid_direction = a.direction != EFeaturePort::OUTPUT || b.direction == EFeaturePort::OUTPUT;
            const bool mismatch = a.description != b.description;
            if (invalid_direction || mismatch || work[to].incoming[destination] != SIZE_MAX)
            {
                return cxx::unexpected(failure(mismatch ? kFeatureResourceMismatch : kFeatureContract, to, c));
            }
            work[to].incoming[destination] = c;
        }
        RenderGraphBuilder builder;
        for (std::size_t e = 0; e < external.size(); ++e)
        {
            const auto& input = external[e];
            const auto i = input.destination.contribution;
            if (i >= work.size())
            {
                return cxx::unexpected(failure(kFeatureContract, i, e));
            }
            const auto p = portIndex(work[i], input.destination.port);
            if (p == work[i].declarations.size())
            {
                return cxx::unexpected(failure(kFeatureContract, i, e));
            }
            const auto& port = work[i].declarations[p].declaration;
            const bool invalid = port.direction == EFeaturePort::OUTPUT ||
                                 input.resource.origin != EGraphOrigin::IMPORTED ||
                                 input.resource.description != port.description || work[i].incoming[p] != SIZE_MAX ||
                                 work[i].resolved[p].version.resource.index != kGraphInvalidIndex;
            if (invalid)
            {
                return cxx::unexpected(failure(kFeatureResourceMismatch, i, p));
            }
            auto resource = builder.declareResource(input.resource);
            if (!resource)
            {
                return cxx::unexpected(resource.error());
            }
            work[i].resolved[p].version = {*resource, 0};
        }
        for (auto i : order)
        {
            for (std::size_t p = 0; p < work[i].declarations.size(); ++p)
            {
                auto& port = work[i].declarations[p];
                auto& d = port.declaration;
                if (d.direction != EFeaturePort::OUTPUT)
                {
                    if (d.direction == EFeaturePort::INPUT && work[i].incoming[p] == SIZE_MAX &&
                        work[i].resolved[p].version.resource.index == kGraphInvalidIndex)
                    {
                        return cxx::unexpected(failure(kFeatureMissingProvider, i, p));
                    }
                    continue;
                }
                auto resource = builder.declareResource(
                    {featureKey(inputs[i].descriptor->type, inputs[i].contribution_key, d.key),
                     d.description,
                     d.origin,
                     d.initial_ranges,
                     d.initial_use}
                );
                if (!resource)
                {
                    return cxx::unexpected(resource.error());
                }
                work[i].resolved[p].version = {*resource, 0};
                if (port.storage)
                {
                    port.storage->push_back({*resource, std::move(*d.storage)});
                }
            }
        }
        std::size_t remaining = work.size();
        while (remaining != 0)
        {
            bool progressed = false;
            for (auto i : order)
            {
                auto& contribution = work[i];
                if (contribution.built)
                {
                    continue;
                }
                const bool ready = std::ranges::all_of(
                    contribution.incoming,
                    [&](auto connection) noexcept
                    { return connection == SIZE_MAX || work[connections[connection].source.contribution].built; }
                );
                if (!ready)
                {
                    continue;
                }
                for (std::size_t p = 0; p < contribution.incoming.size(); ++p)
                {
                    const auto incoming = contribution.incoming[p];
                    if (incoming != SIZE_MAX)
                    {
                        const auto& source = connections[incoming].source;
                        contribution.resolved[p].version =
                            work[source.contribution]
                                .resolved[portIndex(work[source.contribution], source.port)]
                                .version;
                    }
                }
                const auto& input = inputs[i];
                const auto first = builder.passCount();
                const auto first_output = builder.outputCount();
                contribution.first_argument = result.arguments.size();
                contribution.first_binding = result.bindings.size();
                contribution.first_resource = builder.resourceCount();
                contribution.supplied.clear();
                for (const auto& port : contribution.resolved)
                {
                    contribution.supplied.push_back(port.version);
                }
                FeatureBuildContext context{builder, contribution.resolved, result.arguments, result.bindings};
                if (input.graph->build)
                {
                    auto built = input.graph->build(
                        input.config ? input.config->data() : nullptr,
                        input.contribution_key,
                        context
                    );
                    if (!built)
                    {
                        return cxx::unexpected(built.error());
                    }
                }
                const bool changed_prefix =
                    builder.passCount() < first || builder.resourceCount() < contribution.first_resource ||
                    builder.outputCount() != first_output || result.arguments.size() < contribution.first_argument ||
                    result.bindings.size() < contribution.first_binding;
                if (changed_prefix)
                {
                    return cxx::unexpected(failure(kFeatureOwnership, i));
                }
                contribution.resource_count = builder.resourceCount() - contribution.first_resource;
                contribution.argument_count = result.arguments.size() - contribution.first_argument;
                contribution.binding_count = result.bindings.size() - contribution.first_binding;
                for (std::size_t p = 0; p < contribution.resolved.size(); ++p)
                {
                    const auto before = contribution.supplied[p], after = contribution.resolved[p].version;
                    const auto direction = contribution.declarations[p].declaration.direction;
                    const bool invalid_publish =
                        before.resource != after.resource || (direction != EFeaturePort::OUTPUT && before != after);
                    if (invalid_publish)
                    {
                        return cxx::unexpected(failure(kFeatureVersion, i, p));
                    }
                }
                result.contributions.push_back({i, first, builder.passCount() - first});
                contribution.built = true;
                --remaining;
                progressed = true;
            }
            if (!progressed)
            {
                return cxx::unexpected(RenderError{kGraphDependencyCycle});
            }
        }
        for (const auto& output : exports)
        {
            const auto i = output.source.contribution;
            if (i >= work.size())
            {
                return cxx::unexpected(failure(kFeatureContract, i));
            }
            const auto p = portIndex(work[i], output.source.port);
            if (p == work[i].declarations.size() ||
                work[i].declarations[p].declaration.direction != EFeaturePort::OUTPUT)
            {
                return cxx::unexpected(failure(kFeatureContract, i, p));
            }
            auto exported =
                builder.exportOutput({work[i].resolved[p].version, output.range, output.target, output.final_use});
            if (!exported)
            {
                return cxx::unexpected(exported.error());
            }
        }
        auto graph = std::move(builder).finish();
        if (!graph)
        {
            return cxx::unexpected(graph.error());
        }
        std::vector<bool> storage_resources(graph->resources().size(), false);
        const auto markStorage = [&](const auto& group)
        {
            for (const auto& binding : group)
            {
                storage_resources[binding.resource.index] = true;
            }
        };
        markStorage(result.external_bindings.global);
        markStorage(result.external_bindings.scene);
        markStorage(result.external_bindings.view);
        markStorage(result.external_bindings.attachment);
        std::vector<bool> configured(graph->resources().size(), false), constructed(graph->resources().size(), false);
        // Audit only appended contributions, using captured IDs and supplied versions, never names.
        for (const auto& contribution : result.contributions)
        {
            const auto i = contribution.input_index;
            const auto& owner = work[i];
            const auto allowed = [&](ResourceVersion version, bool write)
            {
                const auto resource = version.resource.index;
                if (resource >= owner.first_resource && resource - owner.first_resource < owner.resource_count)
                {
                    return true;
                }
                for (std::size_t p = 0; p < owner.supplied.size(); ++p)
                {
                    if (owner.supplied[p].resource != version.resource)
                    {
                        continue;
                    }
                    if (owner.declarations[p].declaration.direction == EFeaturePort::OUTPUT)
                    {
                        return true;
                    }
                    if (!write && owner.supplied[p] == version)
                    {
                        return true;
                    }
                }
                return false;
            };
            for (auto a = owner.first_argument; a < owner.first_argument + owner.argument_count; ++a)
            {
                const auto resource = result.arguments[a].resource;
                const bool valid = resource.index < storage_resources.size() && storage_resources[resource.index] &&
                                   !configured[resource.index] && allowed({resource, 0}, true);
                if (!valid)
                {
                    return cxx::unexpected(failure(kFeatureOwnership, i, resource.index));
                }
                configured[resource.index] = true;
            }
            for (auto b = owner.first_binding; b < owner.first_binding + owner.binding_count; ++b)
            {
                const auto& binding = result.bindings[b];
                const auto resource = binding.resource;
                const bool valid = resource.index < storage_resources.size() && !storage_resources[resource.index] &&
                                   !constructed[resource.index] &&
                                   graph->resources()[resource.index].origin == EGraphOrigin::IMPORTED &&
                                   binding.create && allowed({resource, 0}, true);
                if (!valid)
                {
                    return cxx::unexpected(failure(kFeatureOwnership, i, resource.index));
                }
                constructed[resource.index] = true;
            }
            for (auto p = contribution.first_pass; p < contribution.first_pass + contribution.pass_count; ++p)
            {
                const auto& pass = graph->passes()[p];
                const PassInterface* input = nullptr;
                if (const auto* raster = std::get_if<RasterWork>(&pass.work))
                {
                    input = &raster->interface;
                }
                if (const auto* compute = std::get_if<ComputeWork>(&pass.work))
                {
                    input = &compute->interface;
                }
                if (input)
                {
                    for (const auto& field : input->parameters.fields)
                    {
                        if (field.resource.index == kGraphInvalidIndex)
                        {
                            continue;
                        }
                        const bool supplied = std::ranges::any_of(
                            owner.supplied,
                            [&](const auto& value) { return value.resource == field.resource; }
                        );
                        if (!supplied && !allowed({field.resource, 0}, true))
                        {
                            return cxx::unexpected(failure(kFeatureOwnership, i, p));
                        }
                    }
                }
                for (const auto& access : pass.uses)
                {
                    const bool write = access.access != EGraphAccess::READ;
                    const bool invalid =
                        !allowed(access.version, write) || (access.fallback && !allowed(*access.fallback, false));
                    if (invalid)
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, p));
                    }
                }
            }
        }
        auto analysis = analyzeRenderGraph(*graph);
        if (!analysis)
        {
            return cxx::unexpected(analysis.error());
        }
        result.definition = std::move(*graph);
        result.analysis = std::move(*analysis);
        return result;
    }

    std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept
    {
        using error::EArgument;
        static constexpr auto args = std::array{EArgument::UNSIGNED, EArgument::UNSIGNED, EArgument::UNSIGNED};
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.feature.contract", "Invalid feature contract", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.duplicate", "Duplicate feature declaration", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.missing_provider", "Required provider missing", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.resource_mismatch", "Resource contracts differ", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.ownership", "Unauthorized graph contribution", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.version", "Unsupported shared version", error::ERecovery::PERMANENT, args}
        };
        return descriptors;
    }
} // namespace lux::render
