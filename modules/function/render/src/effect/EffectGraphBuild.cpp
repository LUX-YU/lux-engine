#include <lux/engine/render/commands/CommandRegistry.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/effect/EffectValidation.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/runtime/RenderEffect.hpp>

namespace lux::render::runtime_detail
{
    namespace
    {
        using effect_detail::ValidatedEffectGraph;
        using effect_detail::ValidatedEffectNode;

        [[nodiscard]] RenderError reportEffectFailure(
            const RenderErrorSink& sink, RenderError error, const EffectDiagnostic& diagnostic,
            std::string_view message
        ) noexcept
        {
            if (sink.report)
            {
                sink.report(sink.context, {ERenderDiagnosticSeverity::ERROR_MESSAGE, error, message, {}, 0, &diagnostic});
            }
            return error;
        }

        [[nodiscard]] RenderResult<ValidatedEffectGraph>
        validateForBuild(const EffectGraph& source, const EffectCatalog& catalog, const RenderErrorSink& sink) noexcept
        {
            auto result = effect_detail::validateEffectGraph(source, catalog);
            if (result)
            {
                return std::move(*result);
            }
            const auto code = result.error().issue == EEffectIssue::CYCLE ? kCapabilityCycle : kCapabilityDeclaration;
            return cxx::unexpected(reportEffectFailure(sink, {code}, result.error(), "Effect graph validation failed"));
        }

        class NodeInitialization final : public EffectInitializationContext
        {
        public:
            NodeInitialization(graph::NodeId node, RenderEffect& owner, std::size_t& config_budget) noexcept
                : node_(node), owner_(owner), config_budget_(config_budget)
            {
            }

            graph::NodeId nodeId() const noexcept override { return node_; }

            RenderResult<void> setConfiguration(ConfigValue configuration) noexcept override
            {
                if (has_configuration_) return cxx::unexpected(RenderError{kCapabilityConflict, {node_.value}});
                const auto bytes = configuration.retainedBytes();
                if (!configuration || bytes > config_budget_)
                    return cxx::unexpected(RenderError{kInvalidConfig, {node_.value}});
                owner_.node_configurations.push_back({node_, std::move(configuration)});
                config_budget_ -= bytes;
                has_configuration_ = true;
                return {};
            }

            const RenderEffect::NodeState* state() const noexcept
            {
                return has_state_ ? &owner_.node_states.back() : nullptr;
            }

        private:
            RenderResult<void> adoptState(
                void* value, void (*destroy)(void*) noexcept, cxx::basic_type_info type, std::size_t bytes
            ) noexcept override
            {
                if (has_state_) return cxx::unexpected(RenderError{kCapabilityConflict});
                owner_.node_states.push_back({
                    std::unique_ptr<void, RenderEffect::NodeState::Destroy>{value, destroy}, type, node_, bytes
                });
                has_state_ = true;
                return {};
            }

            graph::NodeId node_;
            RenderEffect& owner_;
            std::size_t& config_budget_;
            bool has_state_ = false;
            bool has_configuration_ = false;
        };

        struct ImportSlots final
        {
            std::vector<detail::TargetImageSlot> targets;
            std::vector<detail::StorageBindingSlot> storages;
            std::vector<GraphStorageSnapshotId> snapshots;
            std::vector<std::uint8_t> used_storages;
        };

        struct NodePinView final
        {
            std::span<const effect_detail::ValidatedEffectPin> declarations;
            std::span<EffectResourceBundle> outputs;
            std::span<const std::vector<EffectResourceBundle>> inputs;
        };

        struct EffectLoweringWorkspace final
        {
            EffectBuildContract contract;
            RenderGraphBuilder builder;
            std::vector<EffectResourceBundle> bundles;
            std::vector<std::vector<EffectResourceBundle>> node_inputs;
            ImportSlots slots;
            PreparationDefinition preparation;
            std::vector<UniformBlockDeclaration> uniforms;

            EffectLoweringWorkspace(EffectBuildContract contract, std::size_t pin_count) noexcept
                : contract(contract), bundles(pin_count)
            {
                slots.storages.resize(contract.imports.storages.size());
                slots.snapshots.resize(contract.imports.storages.size());
                slots.used_storages.resize(contract.imports.storages.size());
            }

            RenderResult<ResourceVersion>
            targetImage(std::string_view name, rdesc::ETextureFormat format, std::uint32_t samples) noexcept
            {
                if (name.empty()) return cxx::unexpected(RenderError{kCommandContract});
                const auto found = std::ranges::find(slots.targets, name, &detail::TargetImageSlot::binding);
                if (found != slots.targets.end())
                {
                    if (found->format != format || found->samples != samples)
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    return ResourceVersion{found->resource, 0};
                }
                GraphImageDesc image{0, 0, 1, 1, format, samples};
                image.extent.source = EImageExtentSource::IMPORTED;
                auto declared = builder.declareResource({
                    "target-import/" + std::string{name}, image, EGraphOrigin::IMPORTED,
                    {GraphImageRange{}}, EGraphUse::SAMPLED
                });
                if (!declared) return cxx::unexpected(declared.error());
                slots.targets.push_back({std::string{name}, format, samples, *declared});
                return ResourceVersion{*declared, 0};
            }

            RenderResult<ResourceVersion> importStorageResource(std::size_t index, GraphResource declaration) noexcept
            {
                const auto& storages = contract.imports.storages;
                auto& slot = slots.storages[index];
                if (slot.resource.index == kGraphInvalidIndex)
                {
                    const auto range = declaration.output_range;
                    declaration.key = "storage-import/" + storages[index].binding;
                    auto declared = builder.declareResource(std::move(declaration));
                    if (!declared) return cxx::unexpected(declared.error());
                    slot = {*declared, range};
                }
                else if (declaration.output_range != slot.output_range)
                    return cxx::unexpected(RenderError{kStorageMismatch, {index}});
                slots.used_storages[index] = 1;
                return ResourceVersion{slot.resource, 0};
            }

            GraphStorageSnapshotId storageSnapshot(std::size_t index, cxx::basic_type_info type) noexcept
            {
                const auto source = contract.imports.storages[index].preparation_source;
                auto& slot = slots.snapshots[source];
                if (slot.index == kGraphInvalidIndex)
                {
                    slot.index = static_cast<std::uint32_t>(preparation.snapshots.size());
                    preparation.snapshots.push_back({type, source});
                }
                slots.used_storages[index] = 1;
                return slot;
            }

            RenderResult<EffectGraphBuild> finish(const ValidatedEffectGraph&) && noexcept;

            NodePinView bindPins(const ValidatedEffectGraph& source, const ValidatedEffectNode& node) noexcept
            {
                const auto declarations = node.pins.view(source.pins);
                node_inputs.resize(declarations.size());
                for (std::size_t index = 0; index < declarations.size(); ++index)
                {
                    auto& input = node_inputs[index];
                    const auto upstream = declarations[index].sources.view(source.input_sources);
                    input.resize(upstream.size());
                    for (std::size_t pin = 0; pin < upstream.size(); ++pin) input[pin] = bundles[upstream[pin]];
                }
                return {declarations, std::span(bundles).subspan(node.pins.first, node.pins.count), node_inputs};
            }
        };

        class NodeBuild final : public EffectBuildContext
        {
        public:
            NodeBuild(
                EffectLoweringWorkspace& workspace, const ValidatedEffectNode& node, const ConfigValue* configuration,
                NodePinView pins
            ) noexcept : workspace_(workspace), node_(node), configuration_(configuration), pins_(pins)
            {
            }

            graph::NodeId nodeId() const noexcept override { return node_.source; }
            const ConfigValue* nodeConfiguration() const noexcept override { return configuration_; }
            const EffectOutputDesc& target() const noexcept override { return workspace_.contract.target; }
            const EffectBuildDevice& device() const noexcept override { return workspace_.contract.device; }
            RenderGraphBuilder& builder() noexcept override { return workspace_.builder; }

            RenderResult<ResourceVersion> importTargetImage(
                std::string_view binding, rdesc::ETextureFormat format, std::uint32_t samples
            ) noexcept override
            {
                return workspace_.targetImage(binding, format, samples);
            }

            RenderResult<ResourceVersion>
            bindStorageProjection(
                std::string_view binding, const ResourceRequirement& required, GraphResource declaration,
                std::uint32_t projection
            ) noexcept override
            {
                const auto& storages = workspace_.contract.imports.storages;
                const auto found = std::ranges::lower_bound(storages, binding, {}, &EffectStorageImport::binding);
                if (found == storages.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kStorageIdentity, {node_.source.value}});
                const auto* contract = std::get_if<detail::StorageGraphImport>(&found->contract);
                const bool wrong_owner = found->type != required.type || found->projection != projection ||
                    declaration.origin != EGraphOrigin::IMPORTED;
                const bool wrong_shape = !contract || declaration.description != contract->description;
                const bool wrong_boundary = contract && (declaration.initial_ranges != contract->initial_ranges ||
                    declaration.initial_use != contract->initial_use);
                const bool unauthorized_output = declaration.output_range &&
                    (!contract || !contract->final_buffer_output || contract->read_use.has_value());
                if (wrong_owner || wrong_shape || wrong_boundary || unauthorized_output)
                    return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});

                return workspace_.importStorageResource(found - storages.begin(), std::move(declaration));
            }

            RenderResult<GraphStorageSnapshotId>
            importStorageValue(
                std::string_view binding, const ResourceRequirement& required, cxx::basic_type_info type
            ) noexcept override
            {
                const auto& storages = workspace_.contract.imports.storages;
                const auto found = std::ranges::lower_bound(storages, binding, {}, &EffectStorageImport::binding);
                if (found == storages.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kStorageIdentity, {node_.source.value}});

                const auto* contract = std::get_if<EffectStorageImport::SnapshotContract>(&found->contract);
                const bool wrong_type = !contract || contract->hash != type.hash() || contract->name != type.name();
                if (found->type != required.type || wrong_type)
                    return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});

                return workspace_.storageSnapshot(found - storages.begin(), type);
            }

            RenderResult<GraphRasterTextureSetId> importRasterTextures(std::string_view binding) noexcept override
            {
                auto slot = importRasterOwner(binding, EffectStorageImport::ERasterOwner::TEXTURES,
                    workspace_.preparation.texture_sets);
                if (!slot) return cxx::unexpected(slot.error());
                return GraphRasterTextureSetId{*slot};
            }

            RenderResult<GraphRasterMeshSetId> importRasterMeshes(std::string_view binding) noexcept override
            {
                auto slot = importRasterOwner(binding, EffectStorageImport::ERasterOwner::MESHES,
                    workspace_.preparation.mesh_sets);
                if (!slot) return cxx::unexpected(slot.error());
                return GraphRasterMeshSetId{*slot};
            }

            RenderResult<std::optional<GraphRasterTextureSetId>>
            importOptionalRasterTextures(std::string_view binding) noexcept override
            {
                const auto& storages = workspace_.contract.imports.storages;
                if (!std::ranges::binary_search(storages, binding, {}, &EffectStorageImport::binding))
                    return std::nullopt;
                auto slot = importRasterTextures(binding);
                if (!slot) return cxx::unexpected(slot.error());
                return *slot;
            }

            RenderResult<GraphRasterMaterialSetId> importRasterMaterials(std::string_view binding) noexcept override
            {
                auto slot = importRasterOwner(binding, EffectStorageImport::ERasterOwner::MATERIALS,
                    workspace_.preparation.material_sets);
                if (!slot) return cxx::unexpected(slot.error());
                return GraphRasterMaterialSetId{*slot};
            }

            RenderResult<std::uint32_t> importRasterOwner(
                std::string_view binding, EffectStorageImport::ERasterOwner kind, std::vector<std::uint32_t>& sets
            ) noexcept
            {
                const auto& storages = workspace_.contract.imports.storages;
                const auto found = std::ranges::lower_bound(storages, binding, {}, &EffectStorageImport::binding);
                if (found == storages.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kStorageIdentity, {node_.source.value}});
                const auto* owner = std::get_if<EffectStorageImport::ERasterOwner>(&found->contract);
                if (!owner || *owner != kind)
                    return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});

                workspace_.slots.used_storages[found - storages.begin()] = 1;
                const auto previous = std::ranges::find(sets, found->preparation_source);
                const auto index = static_cast<std::uint32_t>(previous - sets.begin());
                if (previous == sets.end()) sets.push_back(found->preparation_source);
                return index;
            }

            RenderResult<ResourceVersion> declarePreparedBuffer(
                std::string_view name, std::uint64_t minimum, std::uint64_t maximum, EGraphUse initial
            ) noexcept override
            {
                const bool invalid_use = initial != EGraphUse::STORAGE_READ && initial != EGraphUse::INDIRECT &&
                    initial != EGraphUse::VERTEX && initial != EGraphUse::INDEX;
                if (invalid_use) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node_.source.value}});
                return declarePreparationBuffer(name, minimum, maximum, initial);
            }

            RenderResult<ResourceVersion>
            declareFrameBuffer(std::string_view name, std::uint64_t minimum, std::uint64_t maximum) noexcept override
            {
                return declarePreparationBuffer(name, minimum, maximum, EGraphUse::UNSPECIFIED);
            }

            RenderResult<ResourceVersion> declarePreparationBuffer(
                std::string_view name, std::uint64_t minimum, std::uint64_t maximum, EGraphUse initial
            ) noexcept
            {
                const bool invalid_capacity = !minimum || minimum > maximum || maximum > UINT64_MAX / 2;
                const bool invalid_alignment = minimum % 4 || maximum % 4;
                if (name.empty() || invalid_capacity || invalid_alignment)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node_.source.value}});
                const bool upload = initial != EGraphUse::UNSPECIFIED;
                std::vector<VGraphRange> initialized;
                if (upload) initialized.push_back(GraphBufferRange{0, minimum});
                auto resource = workspace_.builder.declareResource({
                    "prepared/" + std::to_string(node_.source.value) + "/" + std::string{name},
                    GraphBufferDesc{minimum}, upload ? EGraphOrigin::IMPORTED : EGraphOrigin::FRAME_BUFFER,
                    std::move(initialized), initial
                });
                if (!resource) return cxx::unexpected(resource.error());
                workspace_.preparation.buffers.push_back({*resource, node_.source, minimum, maximum, upload});
                return ResourceVersion{*resource, 0};
            }

            RenderResult<void>
            declareInlineCopy(GraphPassId pass, extension_detail::InlineBlockInput input) noexcept override
            {
                workspace_.preparation.inline_copies.push_back({node_.source, pass, input});
                return {};
            }

            RenderResult<void> addPreparation(GraphPreparation recipe) noexcept override
            {
                if (workspace_.preparation.producers.size() >= kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                workspace_.preparation.producers.push_back({std::move(recipe), node_.source});
                return {};
            }

            std::span<const EffectResourceBundle> inputs(graph::PinSemanticId semantic) const noexcept override
            {
                const auto index = portIndex(semantic);
                if (index == pins_.declarations.size()) return {};
                return pins_.inputs[index];
            }

            bool outputDemanded(graph::PinSemanticId semantic) const noexcept override
            {
                const auto index = portIndex(semantic);
                return index < pins_.declarations.size() && pins_.declarations[index].demanded;
            }

            RenderResult<void> publish(graph::PinSemanticId semantic, EffectResourceBundle bundle) noexcept override
            {
                const auto index = portIndex(semantic);
                const bool is_demanded = index < pins_.declarations.size() && pins_.declarations[index].demanded;
                if (!is_demanded || bundle.empty()) return cxx::unexpected(RenderError{kCapabilityDeclaration});
                auto& published = pins_.outputs[index];
                if (!published.empty()) return cxx::unexpected(RenderError{kCapabilityConflict});
                std::ranges::sort(bundle, {}, &EffectResourceValue::member);
                const auto resource_count = workspace_.builder.resourceCount();
                for (std::size_t i = 0; i < bundle.size(); ++i)
                {
                    const bool duplicate = i != 0 && bundle[i - 1].member == bundle[i].member;
                    const bool unknown_resource = bundle[i].version.resource.index >= resource_count;
                    if (!bundle[i].member.valid() || duplicate || unknown_resource)
                        return cxx::unexpected(RenderError{kCapabilityDeclaration});
                }
                published = std::move(bundle);
                return {};
            }

            [[nodiscard]] bool complete() const noexcept
            {
                for (std::size_t index = 0; index < pins_.declarations.size(); ++index)
                {
                    if (pins_.declarations[index].demanded && pins_.outputs[index].empty()) return false;
                }
                return true;
            }

        private:
            RenderResult<ResourceVersion>
            declareUniformBlock(std::string_view name, extension_detail::EffectUniformSource source) noexcept override
            {
                const auto existing = std::ranges::find_if(workspace_.uniforms, [&](const auto& old) noexcept
                {
                    return old.node == node_.source && old.name == name;
                });
                if (name.empty() || existing != workspace_.uniforms.end())
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node_.source.value}});
                const auto bytes = source.block.bytes;
                auto resource = workspace_.builder.declareResource({
                    "uniform/" + std::to_string(node_.source.value) + "/" + std::string(name),
                    GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, bytes}}, EGraphUse::UNIFORM_READ
                });
                if (!resource) return cxx::unexpected(resource.error());
                workspace_.uniforms.push_back({node_.source, std::string(name), *resource, source});
                return ResourceVersion{*resource, 0};
            }

            RenderResult<GraphCpuDataId> declareCpuValue(cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (workspace_.preparation.cpu_data.size() >= kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                const GraphCpuDataId id{static_cast<std::uint32_t>(workspace_.preparation.cpu_data.size())};
                workspace_.preparation.cpu_data.push_back({type, bytes});
                return id;
            }

            [[nodiscard]] std::size_t portIndex(graph::PinSemanticId semantic) const noexcept
            {
                const auto& ports = node_.capability->ports;
                return std::ranges::find(ports, semantic, &EffectPortDefinition::semantic) - ports.begin();
            }

            EffectLoweringWorkspace& workspace_;
            const ValidatedEffectNode& node_;
            const ConfigValue* configuration_;
            NodePinView pins_;
        };
    }

    RenderResult<void> initializeEffectGraph(
        RenderEffect& candidate, std::unique_ptr<EffectGraph> graph, const EffectCatalog& catalog,
        const CommandRegistry& registry,
        std::size_t max_config_bytes,
        const RenderErrorSink& sink
    ) noexcept
    {
        for (const auto& [id, node] : graph->nodes())
        {
            if (node.arguments.size() > max_config_bytes)
                return cxx::unexpected(RenderError{kInvalidConfig, {id.value}});
            max_config_bytes -= node.arguments.size();
        }
        auto validated = validateForBuild(*graph, catalog, sink);
        if (!validated) return cxx::unexpected(validated.error());
        for (auto index : validated->topological_order)
        {
            const auto& node = validated->nodes[index];
            if (!node.live) continue;
            NodeInitialization context{node.source, candidate, max_config_bytes};
            if (auto initialized = node.implementation->initialize(context, node.arguments); !initialized)
            {
                return cxx::unexpected(reportEffectFailure(
                    sink, initialized.error(), {EEffectIssue::INITIALIZATION_FAILURE, node.source, {}, {}},
                    "Effect node initialization failed"
                ));
            }
            const auto* state = context.state();
            for (auto binding : node.implementation->command_bindings)
            {
                const auto* command = registry.findCommand(binding.command);
                const auto* method = registry.binding(binding);
                const bool is_missing = !command || !method || !state;
                if (is_missing) return cxx::unexpected(RenderError{kCommandContract});
                const bool is_wrong_route = !command->effect || !command->effect_node || command->storage;
                const bool is_wrong_state = method->effect_state_type != state->type || !method->handler.isNull();
                if (is_wrong_route || is_wrong_state || !method->call.invoke)
                    return cxx::unexpected(RenderError{kCommandContract, {node.source.value, commandTag(binding.command)}});
                auto call = method->call;
                call.object = state->value.get();
                candidate.command_routes.push_back({binding.command, call, node.source});
            }
        }
        auto& routes = candidate.command_routes;
        std::ranges::sort(routes, {}, &RenderEffect::CommandRoute::key);
        if (std::ranges::adjacent_find(routes, std::ranges::equal_to{}, &RenderEffect::CommandRoute::key) != routes.end())
            return cxx::unexpected(RenderError{kCapabilityConflict});
        std::ranges::sort(candidate.node_configurations, {}, &EffectNodeConfiguration::node);
        candidate.source = std::move(graph);
        return {};
    }

    RenderResult<EffectGraphBuild>
    lowerEffectGraph(
        EffectRecipeView recipe,
        const EffectCatalog& catalog,
        EffectBuildContract contract,
        const RenderErrorSink& sink
    ) noexcept
    {
        auto validated = validateForBuild(recipe.graph, catalog, sink);
        if (!validated) return cxx::unexpected(validated.error());
        EffectLoweringWorkspace workspace(contract, validated->pins.size());
        const auto configurations = recipe.configurations;
        for (auto index : validated->topological_order)
        {
            const auto& node = validated->nodes[index];
            if (!node.live) continue;
            const auto config = std::ranges::lower_bound(
                configurations, node.source, {}, &EffectNodeConfiguration::node
            );
            const bool configured = config != configurations.end() && config->node == node.source;
            const auto* value = configured ? &config->value : nullptr;
            NodeBuild context(workspace, node, value, workspace.bindPins(*validated, node));
            if (auto built = node.implementation->build(context, node.arguments); !built)
            {
                return cxx::unexpected(reportEffectFailure(
                    sink, built.error(), {EEffectIssue::BUILD_FAILURE, node.source, {}, {}}, "Effect node build failed"
                ));
            }
            if (!context.complete() || workspace.builder.outputCount() != 0)
                return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.source.value}});
        }
        return std::move(workspace).finish(*validated);
    }

    RenderResult<EffectGraphBuild> EffectLoweringWorkspace::finish(const ValidatedEffectGraph& validated) && noexcept
    {
        for (const auto& output : validated.outputs)
        {
            const auto& bundle = bundles[output.pin];
            if (output.target && bundle.size() != 1) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            for (const auto& member : bundle)
            {
                const auto resource = member.version.resource;
                const bool is_uniform =
                    std::ranges::find(uniforms, resource, &UniformBlockDeclaration::resource) != uniforms.end();
                const bool is_prepared =
                    std::ranges::find(preparation.buffers, resource, &PreparationDefinition::Buffer::resource) !=
                    preparation.buffers.end();
                if (is_uniform || is_prepared)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                GraphExport exported{member.version, {}, {}, {}};
                if (output.target)
                {
                    const bool imported_target = std::ranges::find(slots.targets, member.version.resource, &detail::TargetImageSlot::resource) != slots.targets.end();
                    const bool imported_storage = std::ranges::find(
                        slots.storages, member.version.resource, &detail::StorageBindingSlot::resource
                    ) != slots.storages.end();
                    if (imported_target || imported_storage)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    exported.target = GraphOutputRequirements{contract.target.format, contract.target.samples};
                }
                if (auto added = builder.exportOutput(std::move(exported)); !added)
                    return cxx::unexpected(added.error());
            }
        }
        auto source = std::move(builder).finish();
        if (!source) return cxx::unexpected(source.error());
        std::ranges::sort(slots.targets, {}, &detail::TargetImageSlot::binding);
        const bool unused_storage = std::ranges::find(slots.used_storages, 0) != slots.used_storages.end();
        if (unused_storage) return cxx::unexpected(RenderError{kCommandContract});
        std::vector<bool> linked_targets(source->resources().size());
        std::vector<bool> linked_storages(source->resources().size());
        std::vector<bool> linked_buffers(source->resources().size());
        std::vector<const UniformBlockDeclaration*> linked_uniforms(source->resources().size());
        for (const auto& uniform : uniforms) linked_uniforms[uniform.resource.index] = &uniform;
        for (const auto& buffer : preparation.buffers) linked_buffers[buffer.resource.index] = true;
        for (const auto& slot : slots.targets) linked_targets[slot.resource.index] = true;
        std::vector<std::optional<EGraphUse>> storage_read_uses(source->resources().size());
        for (std::size_t i = 0; i < slots.storages.size(); ++i)
        {
            const auto slot = slots.storages[i].resource;
            if (slot.index == kGraphInvalidIndex) continue;
            linked_storages[slot.index] = true;
            const auto& import = std::get<detail::StorageGraphImport>(contract.imports.storages[i].contract);
            storage_read_uses[slot.index] = import.read_use;
        }
        for (std::size_t index = 0; index < source->resources().size(); ++index)
        {
            const auto origin = source->resources()[index].origin;
            const bool linked = linked_targets[index] || linked_storages[index] ||
                linked_uniforms[index] || linked_buffers[index];
            const bool unbound_import = origin == EGraphOrigin::IMPORTED && !linked;
            const bool unbound_frame_buffer = origin == EGraphOrigin::FRAME_BUFFER && !linked_buffers[index];
            if (unbound_import || unbound_frame_buffer)
                return cxx::unexpected(RenderError{kRuntimeUnsupported, {index}});
        }
        for (std::size_t p = 0; p < source->passes().size(); ++p)
        {
            for (const auto& use : source->passes()[p].uses)
            {
                const auto valid = [&](GraphResourceId resource) noexcept
                {
                    if (linked_storages[resource.index] && use.access != EGraphAccess::READ &&
                        !source->resources()[resource.index].output_range) return false;
                    const auto required = storage_read_uses[resource.index];
                    return !required || (use.access == EGraphAccess::READ && use.usage == *required);
                };
                const auto resource = use.version.resource;
                if (!valid(resource) || (use.fallback && !valid(use.fallback->resource)))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, resource.index}});
            }
        }
        auto analysis = analyzePreparedGraph(*source, preparation, contract.device.shader_target);
        if (!analysis) return cxx::unexpected(analysis.error());
        for (const auto pass : analysis->execution_order)
        {
            const auto& graph_pass = source->passes()[pass.index];
            for (const auto& use : graph_pass.uses)
            {
                const auto resource = use.version.resource;
                if (const auto* uniform = linked_uniforms[resource.index])
                {
                    const auto* shader = graph_pass.bindings().shader;
                    const bool typed = shader && std::ranges::any_of(shader->resources, [&](const auto& parameter)
                    {
                        return parameter.access.version.resource == resource &&
                            sameBlockContract(
                                parameter.uniform_constant, uniform->source.block, contract.device.shader_target
                            );
                    });
                    const auto* range = std::get_if<GraphBufferRange>(&use.range);
                    const bool valid_range = range && range->offset == 0 && range->size == uniform->source.block.bytes;
                    const bool valid_use = use.access == EGraphAccess::READ && use.usage == EGraphUse::STORAGE_READ &&
                        !use.fallback && typed;
                    if (!valid_range || !valid_use)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index, resource.index}});
                }
                const bool allowed = use.access == EGraphAccess::READ && use.usage == EGraphUse::SAMPLED;
                if (linked_targets[use.version.resource.index] && !allowed)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index, use.version.resource.index}});
            }
        }
        return EffectGraphBuild{
            std::move(*source), std::move(*analysis), contract.target,
            std::move(slots.targets), std::move(slots.storages),
            std::move(preparation), std::move(uniforms)
        };
    }
}
