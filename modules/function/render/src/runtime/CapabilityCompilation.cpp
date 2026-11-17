#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/extensions/features/FeaturePorts.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

#include <algorithm>
#include <set>

namespace lux::render
{
    namespace
    {
        const FeaturePort* findPort(const FeaturePorts& ports, std::string_view key) noexcept
        {
            const std::array groups{
                &ports.resources,
                &ports.storage.global,
                &ports.storage.scene,
                &ports.storage.view
            };
            for (const auto* group : groups)
            {
                const auto found = std::ranges::find(*group, key, &FeaturePort::key);
                if (found != group->end())
                {
                    return &*found;
                }
            }
            return nullptr;
        }

        RenderResult<void> publishShape(CapabilityPort& published, const FeaturePort& actual) noexcept
        {
            if (const auto* image = std::get_if<GraphImageDesc>(&actual.description))
            {
                auto* shape = std::get_if<CapabilityImageShape>(&published.shape);
                if (!shape)
                {
                    return cxx::unexpected(RenderError{kFeatureResourceMismatch});
                }
                *shape = {image->format, image->samples, image->layers, image->width, image->height};
            }
            else
            {
                auto* shape = std::get_if<CapabilityBufferShape>(&published.shape);
                if (!shape || shape->element_stride == 0)
                {
                    return cxx::unexpected(RenderError{kFeatureResourceMismatch});
                }
                const auto size = std::get<GraphBufferDesc>(actual.description).size;
                if (size % shape->element_stride != 0)
                {
                    return cxx::unexpected(RenderError{kFeatureResourceMismatch});
                }
                shape->minimum_elements = size / shape->element_stride;
            }
            return {};
        }

        RenderResult<void> mergeDependencies(
            std::vector<RenderEffect::ResourceDependency>& result,
            std::vector<RenderEffect::ResourceDependency> dependencies,
            std::uint32_t capability
        ) noexcept
        {
            for (auto& dependency : dependencies)
            {
                for (auto& input : dependency.inputs)
                {
                    input.capability = capability;
                }
                const auto found = std::ranges::find_if(
                    result,
                    [&](const auto& old) noexcept
                    {
                        return old.declaration == dependency.declaration;
                    }
                );
                if (found == result.end())
                {
                    result.push_back(std::move(dependency));
                }
                else
                {
                    found->inputs.insert(found->inputs.end(), dependency.inputs.begin(), dependency.inputs.end());
                }
            }
            return {};
        }

        void appendDefinition(EffectDefinition& result, EffectDefinition source, std::string_view key)
        {
            const auto offset = static_cast<std::uint32_t>(result.features.size());
            for (auto& feature : source.features)
            {
                feature.contribution_key = std::string(key) + "/" + feature.contribution_key;
                result.features.push_back(std::move(feature));
            }
            for (auto& connection : source.connections)
            {
                connection.source.contribution += offset;
                connection.destination.contribution += offset;
                result.connections.push_back(std::move(connection));
            }
            for (auto& input : source.inputs)
            {
                input.destination.contribution += offset;
                result.inputs.push_back(std::move(input));
            }
            // Only the composition's selected outputs are exported. A professional
            // fragment cannot silently add another final target or keep dead passes alive.
        }
    } // namespace

    RenderResult<void>
    RenderServer::Impl::resolveFrameBindings(RenderEffect& effect, const RenderGraph& graph) const noexcept
    {
        for (std::uint32_t occurrence = 0; occurrence < effect.implementations.size(); ++occurrence)
        {
            auto& use = effect.implementations[occurrence];
            const auto& implementation = *capability_registry_.find(use.id);
            const auto& declaration = *capability_registry_.get(capability_registry_.capabilityOf(use.id));
            use.resource_dependencies.assign(declaration.resources.size(), UINT32_MAX);
            for (std::uint32_t index = 0; index < effect.resource_dependencies.size(); ++index)
            {
                for (const auto& input : effect.resource_dependencies[index].inputs)
                {
                    if (input.capability == occurrence)
                    {
                        use.resource_dependencies[input.index] = index;
                    }
                }
            }
            if (std::ranges::find(use.resource_dependencies, UINT32_MAX) != use.resource_dependencies.end())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            if (implementation.view_parameter_type.isValid())
            {
                const bool is_invalid_projection = !implementation.project_parameters ||
                    implementation.view_parameter_type != cxx::make_basic_type_info<CameraInput>();
                if (is_invalid_projection) return cxx::unexpected(RenderError{kFeatureContract});
                effect.view_projections.push_back(
                    {implementation.view_parameter_type, implementation.project_parameters, occurrence}
                );
            }
            for (const auto& input : implementation.draw_inputs)
            {
                const auto* feature = features_.find(implementation.features[input.graph_feature]);
                const auto key = featureKey(
                    FeatureName{feature->name},
                    use.contribution + "/" + std::string(input.contribution_key),
                    input.pass_key
                );
                const auto& passes = graph.passes();
                const auto found =
                    std::ranges::find_if(passes, [&](const auto& pass) noexcept { return pass.key == key; });
                if (found == passes.end())
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                const auto index = static_cast<std::uint32_t>(found - passes.begin());
                const bool is_live = std::ranges::find(effect.analysis.execution_order, GraphPassId{index}) !=
                    effect.analysis.execution_order.end();
                const auto* raster = found->work.getIf<RasterWork>();
                const auto* fixed = raster ? std::get_if<RasterWork::FixedProgram>(&raster->program) : nullptr;
                const bool is_indexed_list = fixed && std::holds_alternative<DrawIndexedList>(fixed->draw);
                const bool is_invalid_draw_input = !is_live || !is_indexed_list;
                if (is_invalid_draw_input) return cxx::unexpected(RenderError{kFeatureContract});
                effect.draw_inputs.push_back(GraphPassId{index});
            }
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::buildCapabilityGraph(
        CreateEffectRequest& request,
        GraphEffectRequest& graph,
        std::vector<RenderEffect::ResourceDependency>& resource_dependencies
    ) noexcept
    {
        auto& composition = request.composition;
        const auto count = composition.contributions.size();
        std::vector<std::string_view> keys;
        std::vector<const CapabilityGraphDescription*> capabilities;
        std::vector<const CapabilityImplementation*> entries;
        for (const auto& selected : composition.contributions)
        {
            const auto identity = capability_registry_.find(selected.capability);
            const auto members = capability_registry_.implementations(identity);
            if (std::ranges::find(members, selected.implementation_id) == members.end())
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
            const auto* capability = capabilityGraph(selected.implementation_id);
            if (!capability)
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            if (auto error = capabilityOutputError(*capability, request.output); error.type != 0)
            {
                return cxx::unexpected(error);
            }
            keys.push_back(selected.key);
            capabilities.push_back(capability);
            entries.push_back(capability_registry_.find(selected.implementation_id));
        }
        std::set<std::pair<std::size_t, std::string_view>> contributed_capabilities;
        for (const auto& selected : composition.contributions)
        {
            const auto type = selected.capability;
            if (type.isValid())
            {
                contributed_capabilities.emplace(type.hash(), type.name());
            }
        }
        for (const auto& selected : composition.contributions)
        {
            const auto id = capability_registry_.find(selected.capability);
            for (const auto prerequisite : capability_registry_.get(id)->prerequisites)
            {
                if (!contributed_capabilities.contains({prerequisite.hash(), prerequisite.name()}))
                {
                    return cxx::unexpected(RenderError{kCapabilityPrerequisite});
                }
            }
        }
        auto connections = resolveCapabilityConnections(keys, capabilities, {}, composition.outputs);
        if (!connections)
        {
            return cxx::unexpected(connections.error());
        }
        std::vector<std::vector<CapabilityPort>> shapes(count);
        std::vector<std::vector<std::vector<FeaturePortRef>>> mapped(count);
        for (auto index : connections->order)
        {
            auto& selected = composition.contributions[index];
            const auto& entry = *entries[index];
            const auto& capability = *capabilities[index];
            ConfigValue decoded;
            auto configuration = qualifyConfiguration(selected.configuration, entry, decoded);
            if (!configuration)
            {
                return cxx::unexpected(configuration.error());
            }
            const auto& declaration = *capability_registry_.get(capability_registry_.find(selected.capability));
            auto dependencies = resolveCapabilityResources(declaration);
            if (!dependencies)
            {
                return cxx::unexpected(dependencies.error());
            }
            if (auto merged = mergeDependencies(resource_dependencies, std::move(*dependencies), index); !merged)
            {
                return merged;
            }
            auto& ports = shapes[index];
            ports = capability.ports;
            std::vector<std::uint8_t> connected(ports.size());
            for (const auto& connection : connections->connections)
            {
                if (connection.destination.contribution == index)
                {
                    const auto& source = shapes[connection.source.contribution][connection.source.port];
                    auto& destination = ports[connection.destination.port];
                    if (!capabilityPortsCompatible(source, destination))
                    {
                        return cxx::unexpected(
                            RenderError{kFeatureResourceMismatch, {index, connection.destination.port}}
                        );
                    }
                    destination.shape = source.shape;
                    connected[connection.destination.port] = 1;
                }
            }
            RenderResult<CapabilityGraphContribution> built =
                cxx::unexpected(RenderError{kRuntimeUnsupported, {index}});
            if (entry.build_graph)
            {
                built = entry.build_graph({request.output, **configuration, ports, connected, entry.features});
            }
            else if (entry.features.size() == 1)
            {
                CapabilityGraphContribution direct;
                auto* typed = std::get_if<ConfigValue>(&selected.configuration);
                direct.definition.features.push_back(
                    {entry.features.front(), "pass", typed ? std::move(*typed) : std::move(decoded)}
                );
                for (const auto& port : ports)
                {
                    direct.ports.push_back({{0, port.key}});
                }
                built = std::move(direct);
            }
            else if (entry.features.empty() && ports.empty())
            {
                built = CapabilityGraphContribution{};
            }
            if (!built)
            {
                return cxx::unexpected(built.error());
            }
            const auto offset = graph.definition.features.size();
            const bool invalid_shape =
                built->ports.size() != ports.size() || built->definition.features.size() > UINT32_MAX - offset;
            if (invalid_shape)
            {
                return cxx::unexpected(RenderError{kFeatureContract, {index}});
            }
            std::vector<FeaturePorts> actual(built->definition.features.size());
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                const auto& feature = built->definition.features[i];
                const auto* registration = features_.find(feature.feature);
                const bool invalid_feature = !registration || !registration->graph.describe_ports ||
                                             registration->config_type != feature.config.type();
                if (invalid_feature)
                {
                    return cxx::unexpected(RenderError{kFeatureContract, {index, i}});
                }
                auto described =
                    registration->graph.describe_ports(feature.config.data(), feature.contribution_key, actual[i]);
                if (!described)
                {
                    return cxx::unexpected(described.error());
                }
            }
            for (std::size_t p = 0; p < ports.size(); ++p)
            {
                auto& references = built->ports[p];
                const bool output = ports[p].direction == ECapabilityPortDirection::OUTPUT;
                if ((output && references.size() != 1) || (connected[p] && references.empty()))
                {
                    return cxx::unexpected(RenderError{kFeatureContract, {index, p}});
                }
                for (auto& reference : references)
                {
                    const auto* port = reference.contribution < actual.size()
                                           ? findPort(actual[reference.contribution], reference.port)
                                           : nullptr;
                    const bool wrong_direction = port && (output ? port->direction != EFeaturePort::OUTPUT
                                                                 : port->direction == EFeaturePort::OUTPUT);
                    if (!port || wrong_direction)
                    {
                        return cxx::unexpected(RenderError{kFeatureContract, {index, p}});
                    }
                    if (output)
                    {
                        auto shape = publishShape(ports[p], *port);
                        if (!shape || !capabilityPortsCompatible(
                                          ports[p],
                                          CapabilityPort{
                                              ports[p].key,
                                              ECapabilityPortDirection::INPUT,
                                              ports[p].semantic,
                                              capability.ports[p].shape,
                                              ports[p].coordinates,
                                              true
                                          }
                                      ))
                        {
                            return cxx::unexpected(RenderError{kFeatureResourceMismatch, {index, p}});
                        }
                    }
                    reference.contribution += static_cast<std::uint32_t>(offset);
                }
            }
            for (auto& feature : built->definition.features)
            {
                feature.parameter_group = index;
            }
            mapped[index] = std::move(built->ports);
            appendDefinition(graph.definition, std::move(built->definition), selected.key);
        }
        for (const auto& connection : connections->connections)
        {
            const auto& source = mapped[connection.source.contribution][connection.source.port].front();
            for (const auto& destination : mapped[connection.destination.contribution][connection.destination.port])
            {
                graph.definition.connections.push_back({source, destination});
            }
        }
        for (auto output : connections->outputs)
        {
            const auto& port = shapes[output.contribution][output.port];
            const auto* image = std::get_if<CapabilityImageShape>(&port.shape);
            const bool is_target = image && image->format == request.output.format &&
                image->samples == request.output.samples;
            // Exported readback buffers and auxiliary images remain Graph roots, not presentation targets.
            // Only an image matching this Effect's Target contract can acquire its physical output.
            std::optional<GraphOutputRequirements> target;
            if (is_target) target = GraphOutputRequirements{request.output.format, request.output.samples};
            graph.definition.outputs.push_back({mapped[output.contribution][output.port].front(), target});
        }
        return {};
    }

    RenderResult<std::vector<RenderEffect::ResourceDependency>> RenderServer::Impl::resolveCapabilityResources(
        const CapabilityDescription& capability
    ) const noexcept
    {
        std::vector<RenderEffect::ResourceDependency> result;
        for (std::uint32_t index = 0; index < capability.resources.size(); ++index)
        {
            const auto& declaration = *capability.resources[index];
            const auto type = resources_->typeId(declaration.type);
            const auto* factory = resources_->factory(type);
            if (!factory) return cxx::unexpected(RenderError{kStorageFactory});
            if (auto valid = compatibleStorage(declaration, *factory->descriptor); !valid)
                return cxx::unexpected(valid.error());
            const auto found = std::ranges::find_if(
                result,
                [&](const auto& dependency) noexcept
                {
                    return dependency.declaration == type;
                }
            );
            if (found == result.end())
            {
                result.push_back(
                    {static_cast<EResourceDomain>(declaration.domain),
                     type,
                     {},
                     {{0, index}}}
                );
            }
            else
            {
                found->inputs.push_back({{}, index});
            }
        }
        return result;
    }

} // namespace lux::render
