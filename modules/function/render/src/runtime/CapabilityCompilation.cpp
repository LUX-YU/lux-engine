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
                &ports.storage.view,
                &ports.storage.attachment
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
            std::vector<RenderEffect::StorageDependency>& result,
            std::vector<RenderEffect::StorageDependency> dependencies,
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
                        return old.scope == dependency.scope && old.requirement.type == dependency.requirement.type &&
                               old.requirement.key == dependency.requirement.key;
                    }
                );
                if (found == result.end())
                {
                    result.push_back(std::move(dependency));
                }
                else
                {
                    if (found->requirement.config != dependency.requirement.config)
                    {
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    }
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
            use.storage_dependencies.assign(implementation.storage_dependencies.size(), UINT32_MAX);
            for (std::uint32_t index = 0; index < effect.storage_dependencies.size(); ++index)
            {
                for (const auto& input : effect.storage_dependencies[index].inputs)
                {
                    if (input.capability == occurrence)
                    {
                        use.storage_dependencies[input.index] = index;
                    }
                }
            }
            if (std::ranges::find(use.storage_dependencies, UINT32_MAX) != use.storage_dependencies.end())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto capability = capability_registry_.capabilityOf(use.id);
            for (auto type : capability_registry_.get(capability)->per_frame_data)
            {
                const auto method = std::ranges::find(implementation.frame_calls, type, &FrameDataCall::data_type);
                const auto& input = *method; // Registration proved exactly one method per public data type.
                auto& call = use.frame_calls.emplace_back(&input, GraphPassId{UINT32_MAX});
                if (input.target == EFrameDataTarget::PARAMETERS)
                {
                    const auto [offset, bytes] = effect.plan->parameterRegion(occurrence);
                    if (!implementation.project_parameters || bytes == 0)
                    {
                        return cxx::unexpected(RenderError{kFeatureContract});
                    }
                    call.parameters = {input.data_type, implementation.project_parameters, offset, bytes};
                    effect.parameter_projections.push_back(call.parameters);
                }
                if (input.target != EFrameDataTarget::DRAW_LIST)
                {
                    continue;
                }
                const auto* feature = features.find(implementation.features[input.graph_feature]);
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
                const auto lowered = std::ranges::find_if(
                    effect.plan->passes(),
                    [index](const auto& pass) noexcept { return pass.source_pass == index; }
                );
                const auto* raster = lowered != effect.plan->passes().end()
                                         ? std::get_if<detail::RasterCommands>(&lowered->work)
                                         : nullptr;
                if (!raster || !std::holds_alternative<detail::IndexedListCommands>(raster->draw))
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                call.pass = GraphPassId{index};
            }
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::buildCapabilityGraph(
        CreateEffectRequest& request,
        GraphEffectRequest& graph,
        std::vector<RenderEffect::StorageDependency>& storage_dependencies
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
            auto dependencies = resolveCapabilityStorage(entry, **configuration);
            if (!dependencies)
            {
                return cxx::unexpected(dependencies.error());
            }
            if (auto merged = mergeDependencies(storage_dependencies, std::move(*dependencies), index); !merged)
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
                const auto* registration = features.find(feature.feature);
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

    RenderResult<std::vector<RenderEffect::StorageDependency>> RenderServer::Impl::resolveCapabilityStorage(
        const CapabilityImplementation& implementation,
        const ConfigValue& configuration
    ) const noexcept
    {
        std::vector<RenderEffect::StorageDependency> result;
        for (std::uint32_t index = 0; index < implementation.storage_dependencies.size(); ++index)
        {
            const auto& declaration = implementation.storage_dependencies[index];
            auto config = declaration.configuration ? declaration.configuration(configuration.data())
                                                    : RenderResult<ConfigValue>{ConfigValue{}};
            if (!config)
            {
                return cxx::unexpected(config.error());
            }
            const auto found = std::ranges::find_if(
                result,
                [&](const auto& dependency) noexcept
                {
                    return dependency.scope == declaration.scope &&
                           dependency.requirement.type.view() == declaration.storage_type &&
                           dependency.requirement.key == declaration.storage_key;
                }
            );
            if (found == result.end())
            {
                result.push_back(
                    {declaration.scope,
                     {StorageName{declaration.storage_type.name()},
                      std::string(declaration.storage_key),
                      std::move(*config)},
                     {},
                     {{0, index}}}
                );
            }
            else
            {
                if (found->requirement.config != *config)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
                found->inputs.push_back({{}, index});
            }
        }
        return result;
    }

} // namespace lux::render
