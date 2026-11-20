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

        class NodeInitialization final : public EffectInitializationContext
        {
        public:
            NodeInitialization(graph::NodeId node, const object::CodeLease& code, RenderEffect& owner) noexcept
                : node_(node), code_(code), owner_(owner)
            {
            }

            graph::NodeId nodeId() const noexcept override { return node_; }

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
                owner_.node_states.push_back({code_, std::unique_ptr<void, RenderEffect::NodeState::Destroy>{
                    value, destroy
                }, type, node_, bytes});
                has_state_ = true;
                return {};
            }

            graph::NodeId node_;
            const object::CodeLease& code_;
            RenderEffect& owner_;
            bool has_state_ = false;
        };

        struct ImportSlots final
        {
            std::vector<GraphResourceId> targets;
            std::vector<GraphResourceId> storages;
            std::vector<GraphStorageSnapshotId> snapshots;
            std::vector<std::uint8_t> used_targets;
            std::vector<std::uint8_t> used_storages;
        };

        class NodeBuild final : public EffectBuildContext
        {
        public:
            NodeBuild(
                const ValidatedEffectGraph& source,
                const ValidatedEffectNode& node,
                const EffectOutputDesc& target,
                const EffectBuildDevice& device,
                RenderGraphBuilder& builder,
                std::span<EffectResourceBundle> bundles,
                const EffectImportContracts& imports,
                ImportSlots& slots,
                PreparationDefinition& preparation,
                std::vector<UniformBlockDeclaration>& uniforms
            ) noexcept : node_(node), target_(target), device_(device), builder_(builder),
                         bundles_(bundles), imports_(imports), slots_(slots), preparation_(preparation),
                         uniforms_(uniforms), pins_(node.pins.view(source.pins)), inputs_(pins_.size())
            {
                for (std::size_t index = 0; index < pins_.size(); ++index)
                {
                    auto& input = inputs_[index];
                    const auto upstream = pins_[index].sources.view(source.input_sources);
                    input.reserve(upstream.size());
                    for (auto pin : upstream) input.push_back(bundles_[pin]);
                }
            }

            graph::NodeId nodeId() const noexcept override { return node_.source; }
            const EffectOutputDesc& target() const noexcept override { return target_; }
            const EffectBuildDevice& device() const noexcept override { return device_; }
            RenderGraphBuilder& builder() noexcept override { return builder_; }

            RenderResult<ResourceVersion> importTargetImage(
                std::string_view binding, rdesc::ETextureFormat format, std::uint32_t samples
            ) noexcept override
            {
                const auto& targets = imports_.targets;
                const auto found = std::ranges::lower_bound(targets, binding, {}, &EffectTargetImport::binding);
                if (found == targets.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kRuntimeIdentity, {node_.source.value}});
                if (found->format != format || found->samples != samples)
                    return cxx::unexpected(RenderError{kDeviceArgument, {node_.source.value}});
                auto& slot = slots_.targets[found->canonical];
                if (slot.index == kGraphInvalidIndex)
                {
                    GraphImageDesc image{0, 0, 1, 1, format, samples};
                    image.extent.source = EImageExtentSource::IMPORTED;
                    auto declared = builder_.declareResource({
                        "target-import/" + targets[found->canonical].binding,
                        image,
                        EGraphOrigin::IMPORTED,
                        {GraphImageRange{}},
                        EGraphUse::SAMPLED
                    });
                    if (!declared) return cxx::unexpected(declared.error());
                    slot = *declared;
                }
                slots_.used_targets[found - targets.begin()] = 1;
                return ResourceVersion{slot, 0};
            }

            RenderResult<ResourceVersion>
            importResource(
                std::string_view binding, const ResourceRequirement& required, GraphResource declaration,
                std::uint32_t projection
            ) noexcept override
            {
                const auto& storages = imports_.storages;
                const auto found = std::ranges::lower_bound(storages, binding, {}, &EffectStorageImport::binding);
                if (found == storages.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kStorageIdentity, {node_.source.value}});
                const auto* contract = std::get_if<detail::StorageGraphImport>(&found->contract);
                const bool wrong_owner = found->type != required.type || found->projection != projection ||
                    declaration.origin != EGraphOrigin::IMPORTED;
                const bool wrong_shape = !contract || declaration.description != contract->description;
                const bool wrong_boundary = contract && (declaration.initial_ranges != contract->initial_ranges ||
                    declaration.initial_use != contract->initial_use);
                if (wrong_owner || wrong_shape || wrong_boundary)
                    return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});

                auto& slot = slots_.storages[found->canonical];
                if (slot.index == kGraphInvalidIndex)
                {
                    declaration.key = "storage-import/" + storages[found->canonical].binding;
                    auto declared = builder_.declareResource(std::move(declaration));
                    if (!declared) return cxx::unexpected(declared.error());
                    slot = *declared;
                }
                slots_.used_storages[found - storages.begin()] = 1;
                return ResourceVersion{slot, 0};
            }

            RenderResult<GraphStorageSnapshotId>
            importStorageValue(
                std::string_view binding, const ResourceRequirement& required, cxx::basic_type_info type
            ) noexcept override
            {
                const auto& storages = imports_.storages;
                const auto found = std::ranges::lower_bound(storages, binding, {}, &EffectStorageImport::binding);
                if (found == storages.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kStorageIdentity, {node_.source.value}});

                const auto* contract = std::get_if<EffectStorageImport::SnapshotContract>(&found->contract);
                const bool wrong_type = !contract || contract->hash != type.hash() || contract->name != type.name();
                if (found->type != required.type || wrong_type)
                    return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});

                auto& slot = slots_.snapshots[found->canonical];
                if (slot.index == kGraphInvalidIndex)
                {
                    slot.index = static_cast<std::uint32_t>(preparation_.snapshots.size());
                    preparation_.snapshots.push_back({type, found->canonical});
                }
                slots_.used_storages[found - storages.begin()] = 1;
                return slot;
            }

            RenderResult<GraphRasterTextureSetId> importRasterTextures(std::string_view binding) noexcept override
            {
                auto slot = importRasterOwner(binding, EffectStorageImport::ERasterOwner::TEXTURES,
                    preparation_.texture_sets);
                if (!slot) return cxx::unexpected(slot.error());
                return GraphRasterTextureSetId{*slot};
            }

            RenderResult<GraphRasterMeshSetId> importRasterMeshes(std::string_view binding) noexcept override
            {
                auto slot = importRasterOwner(binding, EffectStorageImport::ERasterOwner::MESHES,
                    preparation_.mesh_sets);
                if (!slot) return cxx::unexpected(slot.error());
                return GraphRasterMeshSetId{*slot};
            }

            RenderResult<std::optional<GraphRasterTextureSetId>>
            importOptionalRasterTextures(std::string_view binding) noexcept override
            {
                const auto& storages = imports_.storages;
                if (!std::ranges::binary_search(storages, binding, {}, &EffectStorageImport::binding))
                    return std::nullopt;
                auto slot = importRasterTextures(binding);
                if (!slot) return cxx::unexpected(slot.error());
                return *slot;
            }

            RenderResult<GraphRasterMaterialSetId> importRasterMaterials(std::string_view binding) noexcept override
            {
                auto slot = importRasterOwner(binding, EffectStorageImport::ERasterOwner::MATERIALS,
                    preparation_.material_sets);
                if (!slot) return cxx::unexpected(slot.error());
                return GraphRasterMaterialSetId{*slot};
            }

            RenderResult<std::uint32_t> importRasterOwner(
                std::string_view binding, EffectStorageImport::ERasterOwner kind, std::vector<std::uint32_t>& sets
            ) noexcept
            {
                const auto& storages = imports_.storages;
                const auto found = std::ranges::lower_bound(storages, binding, {}, &EffectStorageImport::binding);
                if (found == storages.end() || found->binding != binding)
                    return cxx::unexpected(RenderError{kStorageIdentity, {node_.source.value}});
                const auto* owner = std::get_if<EffectStorageImport::ERasterOwner>(&found->contract);
                if (!owner || *owner != kind)
                    return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});

                slots_.used_storages[found - storages.begin()] = 1;
                const auto previous = std::ranges::find(sets, found->canonical);
                const auto index = static_cast<std::uint32_t>(previous - sets.begin());
                if (previous == sets.end()) sets.push_back(found->canonical);
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
                auto resource = builder_.declareResource({
                    "prepared/" + std::to_string(node_.source.value) + "/" + std::string{name},
                    GraphBufferDesc{minimum}, upload ? EGraphOrigin::IMPORTED : EGraphOrigin::FRAME_BUFFER,
                    std::move(initialized), initial
                });
                if (!resource) return cxx::unexpected(resource.error());
                preparation_.buffers.push_back({*resource, node_.source, minimum, maximum, upload});
                return ResourceVersion{*resource, 0};
            }

            RenderResult<void> addPreparation(GraphPreparation recipe) noexcept override
            {
                if (preparation_.producers.size() >= kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                preparation_.producers.push_back({node_.implementation->code, std::move(recipe), node_.source});
                return {};
            }

            std::span<const EffectResourceBundle> inputs(graph::PinSemanticId semantic) const noexcept override
            {
                const auto index = portIndex(semantic);
                if (index == pins_.size()) return {};
                return inputs_[index];
            }

            bool outputDemanded(graph::PinSemanticId semantic) const noexcept override
            {
                const auto index = portIndex(semantic);
                return index < pins_.size() && pins_[index].demanded;
            }

            RenderResult<void> publish(graph::PinSemanticId semantic, EffectResourceBundle bundle) noexcept override
            {
                const auto index = portIndex(semantic);
                const bool is_demanded = index < pins_.size() && pins_[index].demanded;
                if (!is_demanded || bundle.empty()) return cxx::unexpected(RenderError{kCapabilityDeclaration});
                auto& published = bundles_[node_.pins.first + index];
                if (!published.empty()) return cxx::unexpected(RenderError{kCapabilityConflict});
                std::ranges::sort(bundle, {}, &EffectResourceValue::member);
                for (std::size_t i = 0; i < bundle.size(); ++i)
                {
                    const bool duplicate = i != 0 && bundle[i - 1].member == bundle[i].member;
                    const bool unknown_resource = bundle[i].version.resource.index >= builder_.resourceCount();
                    if (!bundle[i].member.valid() || duplicate || unknown_resource)
                        return cxx::unexpected(RenderError{kCapabilityDeclaration});
                }
                published = std::move(bundle);
                return {};
            }

            [[nodiscard]] bool complete() const noexcept
            {
                for (std::size_t index = 0; index < pins_.size(); ++index)
                {
                    if (pins_[index].demanded && bundles_[node_.pins.first + index].empty()) return false;
                }
                return true;
            }

        private:
            RenderResult<ResourceVersion>
            declareUniformBlock(std::string_view name, extension_detail::EffectUniformSource source) noexcept override
            {
                const auto existing = std::ranges::find_if(uniforms_, [&](const auto& old) noexcept
                {
                    return old.node == node_.source && old.name == name;
                });
                if (name.empty() || existing != uniforms_.end())
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node_.source.value}});
                const auto bytes = source.block.bytes;
                auto resource = builder_.declareResource({
                    "uniform/" + std::to_string(node_.source.value) + "/" + std::string(name),
                    GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, bytes}}, EGraphUse::UNIFORM_READ
                });
                if (!resource) return cxx::unexpected(resource.error());
                uniforms_.push_back({node_.source, std::string(name), *resource, source});
                return ResourceVersion{*resource, 0};
            }

            RenderResult<GraphCpuDataId> declareCpuValue(cxx::basic_type_info type, std::size_t bytes) noexcept override
            {
                if (preparation_.cpu_data.size() >= kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                const GraphCpuDataId id{static_cast<std::uint32_t>(preparation_.cpu_data.size())};
                preparation_.cpu_data.push_back({type, bytes});
                return id;
            }

            [[nodiscard]] std::size_t portIndex(graph::PinSemanticId semantic) const noexcept
            {
                const auto& ports = node_.capability->ports;
                return std::ranges::find(ports, semantic, &EffectPortDefinition::semantic) - ports.begin();
            }

            const ValidatedEffectNode& node_;
            const EffectOutputDesc& target_;
            const EffectBuildDevice& device_;
            RenderGraphBuilder& builder_;
            std::span<EffectResourceBundle> bundles_;
            const EffectImportContracts& imports_;
            ImportSlots& slots_;
            PreparationDefinition& preparation_;
            std::vector<UniformBlockDeclaration>& uniforms_;
            std::span<const effect_detail::ValidatedEffectPin> pins_;
            std::vector<std::vector<EffectResourceBundle>> inputs_;
        };
    }

    RenderResult<void> initializeEffectGraph(
        RenderEffect& candidate, std::unique_ptr<EffectGraph> graph, const EffectCatalog& catalog,
        const CommandRegistry& registry,
        std::size_t max_config_bytes
    ) noexcept
    {
        for (const auto& [id, node] : graph->nodes())
        {
            if (node.arguments.size() > max_config_bytes)
                return cxx::unexpected(RenderError{kInvalidConfig, {id.value}});
            max_config_bytes -= node.arguments.size();
        }
        std::vector<effect_detail::EffectDiagnostic> diagnostics;
        auto validated = effect_detail::validateEffectGraph(*graph, catalog, diagnostics);
        if (!validated) return cxx::unexpected(validated.error());
        for (auto index : validated->topological_order)
        {
            const auto& node = validated->nodes[index];
            if (!node.live) continue;
            NodeInitialization context{node.source, node.implementation->code, candidate};
            if (auto initialized = node.implementation->initialize(context, node.arguments); !initialized)
                return cxx::unexpected(initialized.error());
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
        candidate.source = std::move(graph);
        return {};
    }

    RenderResult<EffectGraphBuild>
    lowerEffectGraph(
        const EffectGraph& graph, const EffectCatalog& catalog, const EffectOutputDesc& target,
        const EffectBuildDevice& device, const EffectImportContracts& imports
    ) noexcept
    {
        std::vector<effect_detail::EffectDiagnostic> diagnostics;
        auto validated = effect_detail::validateEffectGraph(graph, catalog, diagnostics);
        if (!validated) return cxx::unexpected(validated.error());
        RenderGraphBuilder builder;
        std::vector<EffectResourceBundle> bundles(validated->pins.size());
        ImportSlots slots;
        PreparationDefinition preparation;
        std::vector<UniformBlockDeclaration> uniforms;
        slots.targets.resize(imports.targets.size());
        slots.storages.resize(imports.storages.size());
        slots.snapshots.resize(imports.storages.size());
        slots.used_targets.resize(imports.targets.size());
        slots.used_storages.resize(imports.storages.size());
        for (auto index : validated->topological_order)
        {
            const auto& node = validated->nodes[index];
            if (!node.live) continue;
            NodeBuild context{
                *validated, node, target, device, builder, bundles, imports, slots, preparation, uniforms
            };
            if (auto built = node.implementation->build(context, node.arguments); !built)
                return cxx::unexpected(built.error());
            if (!context.complete() || builder.outputCount() != 0)
                return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.source.value}});
        }
        for (const auto& output : validated->outputs)
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
                    const bool imported_target = std::ranges::find(slots.targets, member.version.resource) != slots.targets.end();
                    const bool imported_storage = std::ranges::find(slots.storages, member.version.resource) != slots.storages.end();
                    if (imported_target || imported_storage)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    exported.target = GraphOutputRequirements{target.format, target.samples};
                }
                if (auto added = builder.exportOutput(std::move(exported)); !added)
                    return cxx::unexpected(added.error());
            }
        }
        auto source = std::move(builder).finish();
        if (!source) return cxx::unexpected(source.error());
        const bool unused_target = std::ranges::find(slots.used_targets, 0) != slots.used_targets.end();
        const bool unused_storage = std::ranges::find(slots.used_storages, 0) != slots.used_storages.end();
        if (unused_target || unused_storage) return cxx::unexpected(RenderError{kCommandContract});
        std::vector<bool> linked_targets(source->resources().size());
        std::vector<bool> linked_storages(source->resources().size());
        std::vector<bool> linked_buffers(source->resources().size());
        std::vector<const UniformBlockDeclaration*> linked_uniforms(source->resources().size());
        for (const auto& uniform : uniforms) linked_uniforms[uniform.resource.index] = &uniform;
        for (const auto& buffer : preparation.buffers) linked_buffers[buffer.resource.index] = true;
        for (const auto slot : slots.targets)
        {
            if (slot.index != kGraphInvalidIndex) linked_targets[slot.index] = true;
        }
        std::vector<std::optional<EGraphUse>> storage_read_uses(source->resources().size());
        for (std::size_t i = 0; i < slots.storages.size(); ++i)
        {
            const auto slot = slots.storages[i];
            if (slot.index == kGraphInvalidIndex) continue;
            linked_storages[slot.index] = true;
            storage_read_uses[slot.index] = std::get<detail::StorageGraphImport>(imports.storages[i].contract).read_use;
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
                    const auto required = storage_read_uses[resource.index];
                    return !required || (use.access == EGraphAccess::READ && use.usage == *required);
                };
                const auto resource = use.version.resource;
                if (!valid(resource) || (use.fallback && !valid(use.fallback->resource)))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, resource.index}});
            }
        }
        auto analysis = analyzePreparedGraph(*source, preparation);
        if (!analysis) return cxx::unexpected(analysis.error());
        for (const auto pass : analysis->execution_order)
        {
            const auto& graph_pass = source->passes()[pass.index];
            for (const auto& use : graph_pass.uses)
            {
                const auto resource = use.version.resource;
                if (const auto* uniform = linked_uniforms[resource.index])
                {
                    const auto typed = std::ranges::find_if(graph_pass.interface.resources, [&](const auto& parameter)
                    {
                        return parameter.access.version.resource == resource &&
                            parameter.uniform_constant.type == uniform->source.block.type &&
                            parameter.uniform_constant.layout == uniform->source.block.layout &&
                            parameter.uniform_constant.bytes == uniform->source.block.bytes;
                    });
                    const auto* range = std::get_if<GraphBufferRange>(&use.range);
                    const bool valid_range = range && range->offset == 0 && range->size == uniform->source.block.bytes;
                    const bool valid_use = use.access == EGraphAccess::READ && use.usage == EGraphUse::STORAGE_READ &&
                        !use.fallback && typed != graph_pass.interface.resources.end();
                    if (!valid_range || !valid_use)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index, resource.index}});
                }
                const bool allowed = use.access == EGraphAccess::READ && use.usage == EGraphUse::SAMPLED;
                if (linked_targets[use.version.resource.index] && !allowed)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {pass.index, use.version.resource.index}});
            }
        }
        return EffectGraphBuild{
            std::move(*source), std::move(*analysis), target, std::move(slots.targets), std::move(slots.storages),
            std::move(preparation), std::move(uniforms)
        };
    }
}
