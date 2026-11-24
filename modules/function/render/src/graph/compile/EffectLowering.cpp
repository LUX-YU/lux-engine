#include <numeric>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/graph/compile/EffectDeclarations.hpp>
#include <lux/engine/render/graph/compile/RenderGraphCompiler.hpp>
#include <lux/engine/render/effect/EffectValidation.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/graph/compile/GraphDefinition.hpp>

namespace lux::render::runtime_detail
{
    using effect_detail::ValidatedEffectGraph;
    using effect_detail::ValidatedEffectNode;

    struct EffectLoweringWorkspace final
    {
        const GraphOutputRequirements& target;
        const EffectBuildDevice& device;
        GraphDraftBuilder builder;
        std::vector<EffectResourceBundle> bundles;
        const ValidatedEffectGraph& topology;
        EffectDeclarations& draft;

        EffectLoweringWorkspace(
            const GraphOutputRequirements& target, const EffectBuildDevice& device,
            const ValidatedEffectGraph& graph, EffectDeclarations& declarations
        ) noexcept
            : target(target), device(device), bundles(graph.pins.size()), topology(graph), draft(declarations)
        {
        }

        RenderResult<std::uint32_t> declareStorage(EffectResourceOperand declaration) noexcept
        {
            if (declaration.binding.empty() || !declaration.type.isValid())
                return cxx::unexpected(RenderError{kStorageContract});
            auto& declarations = draft.operands;
            auto found = std::ranges::find(declarations, declaration.binding, &EffectResourceOperand::binding);
            if (found != declarations.end())
            {
                const bool conflicting_sources = found->requirement != kGraphInvalidIndex &&
                    declaration.requirement != kGraphInvalidIndex && found->requirement != declaration.requirement;
                if (found->type != declaration.type || found->projection != declaration.projection || conflicting_sources)
                    return cxx::unexpected(RenderError{kStorageMismatch});
                if (auto* reads = std::get_if<DrawResourceReads>(&found->contract))
                {
                    const auto* input = std::get_if<DrawResourceReads>(&declaration.contract);
                    if (!input)
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    *reads |= *input;
                }
                else if (found->contract != declaration.contract)
                    return cxx::unexpected(RenderError{kStorageMismatch});
                if (found->requirement == kGraphInvalidIndex) found->requirement = declaration.requirement;
                found->optional &= declaration.optional;
                return static_cast<std::uint32_t>(found - declarations.begin());
            }
            if (declarations.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
            const auto index = static_cast<std::uint32_t>(declarations.size());
            declarations.push_back(std::move(declaration));
            return index;
        }

        RenderResult<ResourceVersion>
        targetImage(std::string_view name, rdesc::ETextureFormat format, std::uint32_t samples) noexcept
        {
            if (name.empty()) return cxx::unexpected(RenderError{kCommandContract});
            const auto found = std::ranges::find(draft.target_imports, name, &detail::TargetImageSlot::binding);
            if (found != draft.target_imports.end())
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
            draft.target_imports.push_back({std::string{name}, format, samples, *declared});
            return ResourceVersion{*declared, 0};
        }

        RenderResult<void> orderStorageBindings() noexcept
        {
            const auto count = draft.operands.size();
            std::vector<std::uint32_t> order(count), remap(count);
            std::iota(order.begin(), order.end(), 0u);
            auto& declarations = draft.operands;
            const auto name = [&](auto index) -> std::string_view { return declarations[index].binding; };
            std::ranges::sort(order, {}, name);
            std::vector<EffectResourceOperand> ordered;
            ordered.reserve(count);
            for (const auto index : order)
            {
                remap[index] = static_cast<std::uint32_t>(ordered.size());
                ordered.push_back(std::move(declarations[index]));
            }
            declarations = std::move(ordered);
            for (auto& algorithm : draft.algorithms)
                for (auto& view : algorithm.declaration.resource_views)
                {
                    if (view.index >= count)
                        return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, view.index}});
                    view.index = remap[view.index];
                }
            return {};
        }

        RenderResult<void> buildNode(const ValidatedEffectNode& node) noexcept
        {
            if (!node.configured) return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.source.value}});
            EffectCompileContext context(*this, node);
            auto built = node.configured->onBuild(context);
            if (built && !context.complete()) return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.source.value}});
            return built;
        }

        RenderResult<GraphDefinition> finish(std::optional<EGraphUse> target_final_use) && noexcept;

    };

} // namespace lux::render::runtime_detail

namespace lux::render
{
    using runtime_detail::EffectResourceOperand;

    EffectCompileContext::EffectCompileContext(
        runtime_detail::EffectLoweringWorkspace& workspace, const effect_detail::ValidatedEffectNode& node
    ) noexcept
        : workspace_(workspace), node_(node), pins_(node.pins.view(workspace.topology.pins)),
          outputs_(std::span{workspace.bundles}.subspan(node.pins.first, node.pins.count))
    {
    }

    graph::NodeId EffectCompileContext::nodeId() const noexcept { return node_.source; }

    const GraphOutputRequirements& EffectCompileContext::target() const noexcept { return workspace_.target; }

    const EffectBuildDevice& EffectCompileContext::device() const noexcept { return workspace_.device; }

    RenderResult<ResourceVersion> EffectCompileContext::importTargetImage(
        std::string_view binding, rdesc::ETextureFormat format, std::uint32_t samples
    ) noexcept
    {
        return workspace_.targetImage(binding, format, samples);
    }

    RenderResult<ResourceVersion>
    EffectCompileContext::importResource(
        std::string_view binding, ResourceNameView owner, ResourceProjection declaration, std::uint32_t projection
    ) noexcept
    {
        if (declaration.final_buffer_output) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        return bindStorageProjection({std::string{binding}, ResourceName{owner.name()}, std::move(declaration), projection}, {});
    }

    RenderResult<std::uint32_t> EffectCompileContext::requireResource(std::string_view requirement) noexcept
    {
        const auto& resources = node_.capability->resources;
        const auto requested = std::ranges::find(resources, requirement, &ResourceRequirement::binding);
        if (requested == resources.end())
            return cxx::unexpected(RenderError{kStorageContract, {node_.source.value}});
        return linkRequirement(*requested);
    }

    RenderResult<std::uint32_t> EffectCompileContext::linkRequirement(const ResourceRequirement& requested) noexcept
    {
        if (!requested.isValid()) return cxx::unexpected(RenderError{kStorageContract, {node_.source.value}});
        const auto& offered = node_.capability->resources;
        const auto match = std::ranges::find(offered, requested.binding, &ResourceRequirement::binding);
        if (match != offered.end() && *match != requested)
            return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});
        auto& manifest = workspace_.draft.requirements;
        const auto found = std::ranges::find(manifest, requested.binding, &runtime_detail::DeclaredResourceRequirement::binding);
        const auto index = static_cast<std::size_t>(found - manifest.begin());
        if (index >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (found == manifest.end())
        {
            manifest.push_back({
                std::string{requested.binding}, ResourceName{requested.type.name()}, requested.domain, requested.lifetime,
                requested.scope_default
            });
        }
        else if (found->type.name() != requested.type.name() || found->domain != requested.domain ||
                 found->lifetime != requested.lifetime || found->scope_default != requested.scope_default)
            return cxx::unexpected(RenderError{kStorageMismatch, {node_.source.value}});
        return static_cast<std::uint32_t>(index);
    }

    RenderResult<ResourceVersion> EffectCompileContext::importProjection(
        const ResourceRequirement& requirement, std::string_view operand, ResourceProjection declaration, std::uint32_t projection
    ) noexcept
    {
        if (declaration.final_buffer_output) return cxx::unexpected(RenderError{kStorageContract, {node_.source.value}});
        const auto index = linkRequirement(requirement);
        if (!index) return cxx::unexpected(index.error());
        const auto& requested = workspace_.draft.requirements[*index];
        EffectResourceOperand input{std::string{operand}, requested.type, std::move(declaration), projection};
        input.requirement = *index;
        return bindStorageProjection(std::move(input), {});
    }

    RenderResult<ResourceVersion>
    EffectCompileContext::importFinalBuffer(
        std::string_view binding, ResourceNameView owner, GraphResource declaration, std::uint32_t projection
    ) noexcept
    {
        const auto* buffer = std::get_if<GraphBufferDesc>(&declaration.description);
        if (!buffer) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        if (declaration.origin != EGraphOrigin::IMPORTED) return cxx::unexpected(RenderError{kStorageContract});
        const auto claim = declaration.output_range.value_or(GraphBufferRange{0, buffer->size});
        ResourceProjection output{
            *buffer, std::move(declaration.initial_ranges), declaration.initial_use, {}, true
        };
        return bindStorageProjection({std::string{binding}, ResourceName{owner.name()}, std::move(output), projection}, claim);
    }

    RenderResult<ResourceVersion>
    EffectCompileContext::bindStorageProjection(EffectResourceOperand input, std::optional<GraphBufferRange> claim) noexcept
    {
        const auto& declared = std::get<ResourceProjection>(input.contract);
        if (declared.initial_use == EGraphUse::UNSPECIFIED)
            return cxx::unexpected(RenderError{kStorageContract, {node_.source.value}});
        if (declared.concurrent_image_read)
        {
            const bool read_layout = declared.initial_use == EGraphUse::SAMPLED ||
                declared.initial_use == EGraphUse::TRANSFER_SOURCE;
            const bool valid = std::holds_alternative<GraphImageDesc>(declared.description) &&
                read_layout && declared.read_use == declared.initial_use;
            if (!valid) return cxx::unexpected(RenderError{kStorageContract, {node_.source.value}});
        }
        const auto index = workspace_.declareStorage(std::move(input));
        if (!index) return cxx::unexpected(index.error());
        auto& storage = workspace_.draft.operands[*index];
        auto& slot = storage.slot;
        if (slot.resource.index == kGraphInvalidIndex)
        {
            const auto& schema = std::get<ResourceProjection>(storage.contract);
            const auto resource = workspace_.builder.declareResource({
                "storage-import/" + storage.binding, schema.description, EGraphOrigin::IMPORTED,
                schema.initial_ranges, schema.initial_use, claim
            });
            if (!resource) return cxx::unexpected(resource.error());
            slot = {*resource, claim};
        }
        else if (claim != slot.output_range)
            return cxx::unexpected(RenderError{kStorageMismatch, {*index}});
        return ResourceVersion{slot.resource, 0};
    }

    RenderResult<ResourceViewSlot> EffectCompileContext::bindResourceView(
        const ResourceRequirement& source, std::string_view operand, cxx::basic_type_info type
    ) noexcept
    {
        auto requirement = linkRequirement(source);
        if (!requirement) return cxx::unexpected(requirement.error());
        EffectResourceOperand input{
            std::string{operand}, ResourceName{source.type.name()},
            EffectResourceOperand::ViewContract{type.hash(), std::string{type.name()}}, 0
        };
        input.requirement = *requirement;
        auto slot = workspace_.declareStorage(std::move(input));
        if (!slot) return cxx::unexpected(slot.error());
        return ResourceViewSlot{*slot};
    }

    RenderResult<ResourceVersion> EffectCompileContext::declarePreparedBuffer(
        std::string_view name, std::uint64_t minimum, std::uint64_t maximum, EGraphUse initial
    ) noexcept
    {
        const bool invalid_use = initial != EGraphUse::STORAGE_READ && initial != EGraphUse::INDIRECT &&
            initial != EGraphUse::VERTEX && initial != EGraphUse::INDEX;
        if (invalid_use) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node_.source.value}});
        return declarePreparationBuffer(name, minimum, maximum, initial);
    }

    RenderResult<ResourceVersion>
    EffectCompileContext::declareFrameBuffer(
        std::string_view name, std::uint64_t minimum, std::uint64_t maximum, std::optional<ResourceBufferExtent> extent
    ) noexcept
    {
        std::uint32_t requirement = kGraphInvalidIndex;
        if (extent)
        {
            auto resolved = requireResource(extent->requirement);
            if (!resolved) return cxx::unexpected(resolved.error());
            requirement = *resolved;
        }
        auto buffer = declarePreparationBuffer(name, minimum, maximum, EGraphUse::UNSPECIFIED);
        if (buffer && extent)
        {
            auto& declaration = workspace_.draft.buffers.back();
            declaration.size_requirement = requirement;
            declaration.size_field = extent->field;
        }
        return buffer;
    }

    RenderResult<ResourceVersion> EffectCompileContext::declarePreparationBuffer(
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
        workspace_.draft.buffers.push_back({*resource, node_.source, minimum, maximum, upload});
        return ResourceVersion{*resource, 0};
    }

    RenderResult<void>
    EffectCompileContext::declareInlineCopy(GraphPassId pass, extension_detail::InlineBlockInput input) noexcept
    {
        workspace_.draft.inline_copies.push_back({&node_, pass, input});
        return {};
    }

    RenderResult<void> EffectCompileContext::declareFrameAlgorithm(
        FrameAlgorithmUses uses, ConfigValue configuration, extension_detail::BindFrameAlgorithm bind
    ) noexcept
    {
        if (workspace_.draft.algorithms.size() >= kGraphInvalidIndex)
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        const auto source = static_cast<std::uint32_t>(workspace_.draft.algorithms.size());
        workspace_.draft.algorithms.push_back({source, std::move(uses), node_.source, std::move(configuration), bind});
        return {};
    }

    std::span<const std::uint32_t> EffectCompileContext::sourcePins(graph::PinSemanticId semantic) const noexcept
    {
        const auto index = portIndex(semantic);
        if (index == pins_.size()) return {};
        return pins_[index].sources.view(workspace_.topology.input_sources);
    }

    std::span<const EffectResourceBundle> EffectCompileContext::sourceBundles() const noexcept
    {
        return workspace_.bundles;
    }

    bool EffectCompileContext::outputDemanded(graph::PinSemanticId semantic) const noexcept
    {
        const auto index = portIndex(semantic);
        return index < pins_.size() && pins_[index].demanded;
    }

    RenderResult<void> EffectCompileContext::publish(graph::PinSemanticId semantic, EffectResourceBundle bundle) noexcept
    {
        const auto index = portIndex(semantic);
        const bool is_demanded = index < pins_.size() && pins_[index].demanded;
        if (!is_demanded || bundle.empty()) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        auto& published = outputs_[index];
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

    bool EffectCompileContext::complete() const noexcept
    {
        for (std::size_t index = 0; index < pins_.size(); ++index)
        {
            if (pins_[index].demanded && outputs_[index].empty()) return false;
        }
        return true;
    }

    RenderResult<ResourceVersion>
    EffectCompileContext::declareConstantBuffer(extension_detail::ConstantBufferField source) noexcept
    {
        const auto existing = std::ranges::find_if(workspace_.draft.uniforms, [&](const auto& old) noexcept
        {
            return old.node == &node_ && old.source.bind == source.bind;
        });
        if (existing != workspace_.draft.uniforms.end()) return ResourceVersion{existing->resource, 0};
        const auto bytes = source.block.bytes;
        auto resource = workspace_.builder.declareResource({
            "constant buffer/" + std::to_string(workspace_.draft.uniforms.size()),
            GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, bytes}}, EGraphUse::UNIFORM_READ
        });
        if (!resource) return cxx::unexpected(resource.error());
        workspace_.draft.uniforms.push_back({&node_, *resource, source});
        return ResourceVersion{*resource, 0};
    }

    std::size_t EffectCompileContext::portIndex(graph::PinSemanticId semantic) const noexcept
    {
        const auto& ports = node_.capability->ports;
        return std::ranges::find(ports, semantic, &EffectPortDefinition::semantic) - ports.begin();
    }

    const VGraphResourceDesc* EffectCompileContext::resourceDescription(GraphResourceId id) const & noexcept
    {
        return workspace_.builder.resourceDescription(id);
    }

    RenderResult<GraphResourceId> EffectCompileContext::declareResource(GraphResource resource) noexcept
    {
        return workspace_.builder.declareResource(std::move(resource));
    }

    RenderResult<HistoryDescription>
    EffectCompileContext::declareHistory(std::string key, HistoryDeclaration declaration) noexcept
    {
        return workspace_.builder.declareHistory(std::move(key), std::move(declaration));
    }

    RenderResult<void>
    EffectCompileContext::publishHistory(ResourceVersion value, std::optional<PassCondition> condition) noexcept
    {
        return workspace_.builder.publishHistory(value, condition);
    }

    RenderResult<PassCondition> EffectCompileContext::sameBinding(GraphResourceId left, GraphResourceId right) noexcept
    {
        return workspace_.builder.sameBinding(left, right);
    }

    RenderResult<std::uint32_t> EffectCompileContext::declareCondition(std::string key) noexcept
    {
        return workspace_.builder.declareCondition(std::move(key));
    }

    RenderResult<std::uint32_t> EffectCompileContext::addProgram(GraphProgram program) noexcept
    {
        return workspace_.builder.addProgram(std::move(program));
    }

    std::uint32_t EffectCompileContext::addSampler(SamplerDescription sampler) noexcept
    {
        return workspace_.builder.addSampler(sampler);
    }

    RenderResult<ResourceVersion> EffectCompileContext::nextWriteVersion(GraphResourceId resource) const noexcept
    {
        return workspace_.builder.nextWriteVersion(resource);
    }

    RenderResult<void> EffectCompileContext::dependsOn(GraphPassId before, GraphPassId after) noexcept
    {
        return workspace_.builder.dependsOn(before, after);
    }

    RenderResult<PassResult>
    EffectCompileContext::addRasterPass(std::string key, RasterWork work, GraphPassOptions options) noexcept
    {
        auto& draft = workspace_.draft;
        const auto before = draft.requirements.size();
        cxx::scope_exit rollback([&]() noexcept { draft.requirements.resize(before); });
        runtime_detail::EffectDeclarations::RasterSource source{};
        const auto* stream = std::get_if<RasterPassTemplate>(&work.program);
        if (stream)
        {
            if (!stream->draw_list.empty())
            {
                auto linked = requireResource(stream->draw_list);
                if (!linked) return cxx::unexpected(linked.error());
                source.draw_list = *linked;
            }
            const auto* ready = std::get_if<RasterPassTemplate::ReadyPrograms>(&stream->programs);
            if (ready && !ready->resource.empty())
            {
                auto linked = requireResource(ready->resource);
                if (!linked) return cxx::unexpected(linked.error());
                source.programs = *linked;
            }
        }
        auto added = workspace_.builder.addRasterPass(std::move(key), std::move(work), options);
        if (!added) return added;
        if (source.draw_list != kGraphInvalidIndex || source.programs != kGraphInvalidIndex)
        {
            source.pass = added->pass;
            draft.raster_sources.push_back(source);
        }
        rollback.release();
        return added;
    }

    RenderResult<PassResult>
    EffectCompileContext::addComputePass(std::string key, ComputeWork work, GraphPassOptions options) noexcept
    {
        return workspace_.builder.addComputePass(std::move(key), std::move(work), options);
    }

    RenderResult<PassResult>
    EffectCompileContext::addComputeSequence(std::string key, std::vector<ComputeWork> work, GraphPassOptions options) noexcept
    {
        return workspace_.builder.addComputeSequence(std::move(key), std::move(work), options);
    }

    RenderResult<PassResult>
    EffectCompileContext::addTransferPass(std::string key, TransferWork work, GraphPassOptions options) noexcept
    {
        return workspace_.builder.addTransferPass(std::move(key), std::move(work), options);
    }

    RenderResult<ResourceVersion>
    EffectCompileContext::declareChoice(PassCondition predicate, ChoiceArm first, ChoiceArm second) noexcept
    {
        auto& draft = workspace_.draft;
        // GraphDraftBuilder rolls back the GPU declarations. Save the node-side declarations that arm callbacks can change.
        auto imports = draft.operands;
        const auto requirement_count = draft.requirements.size();
        const auto raster_source_count = draft.raster_sources.size();
        const auto targets = draft.target_imports.size();
        const auto buffers = draft.buffers.size(), uniforms = draft.uniforms.size();
        const auto copies = draft.inline_copies.size(), calls = draft.algorithms.size();
        std::vector<bool> published;
        published.reserve(outputs_.size());
        for (const auto& output : outputs_) published.push_back(!output.empty());
        auto selected = workspace_.builder.choose(predicate,
            [&](GraphDraftBuilder&) noexcept { return first(*this); },
            [&](GraphDraftBuilder&) noexcept { return second(*this); }
        );
        if (!selected)
        {
            draft.operands = std::move(imports);
            draft.requirements.resize(requirement_count);
            draft.raster_sources.resize(raster_source_count);
            draft.target_imports.resize(targets);
            draft.buffers.resize(buffers);
            draft.uniforms.resize(uniforms);
            draft.inline_copies.resize(copies);
            draft.algorithms.erase(draft.algorithms.begin() + calls, draft.algorithms.end());
            for (std::size_t i = 0; i < published.size(); ++i)
                if (!published[i]) outputs_[i].clear();
        }
        return selected;
    }

} // namespace lux::render

namespace lux::render::detail
{
    RenderResult<GraphDefinition>
    RenderGraphCompiler::lowerEffect(
        const effect_detail::ValidatedEffectGraph& graph, const EffectCompileConfig& config,
        const EffectBuildDevice& device, runtime_detail::EffectDeclarations& declarations
    ) const noexcept
    {
        runtime_detail::EffectLoweringWorkspace workspace(config.output, device, graph, declarations);
        for (auto index : graph.topological_order)
        {
            const auto& node = graph.nodes[index];
            if (!node.live) continue;
            if (auto built = workspace.buildNode(node); !built)
            {
                return cxx::unexpected(effect_detail::reportEffectFailure(
                    environment_.diagnostics, built.error(), {EEffectIssue::BUILD_FAILURE, node.source, {}, {}},
                    "Effect node build failed"
                ));
            }
        }
        return std::move(workspace).finish(config.target_final_use);
    }
} // namespace lux::render::detail

namespace lux::render::runtime_detail
{
    RenderResult<GraphDefinition> EffectLoweringWorkspace::finish(std::optional<EGraphUse> target_final_use) && noexcept
    {
        for (const auto& output : topology.outputs)
        {
            const auto& bundle = bundles[output.pin];
            if (output.target && bundle.size() != 1) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            for (const auto& member : bundle)
            {
                const auto resource = member.version.resource;
                const bool is_uniform =
                    std::ranges::find(draft.uniforms, resource, &ConstantBufferDeclaration::resource) != draft.uniforms.end();
                const bool is_prepared = draft.findBuffer(resource) != nullptr;
                if (is_uniform || is_prepared)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                GraphExport exported{member.version, {}, {}, {}};
                if (output.target)
                {
                    const bool imported_target = std::ranges::find(draft.target_imports, member.version.resource, &detail::TargetImageSlot::resource) != draft.target_imports.end();
                    const bool imported_storage = std::ranges::find(
                        draft.operands, member.version.resource, [](const auto& input) { return input.slot.resource; }
                    ) != draft.operands.end();
                    if (imported_target || imported_storage)
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    exported.target = target;
                    exported.final_use = target_final_use;
                }
                if (auto added = builder.exportOutput(std::move(exported)); !added)
                    return cxx::unexpected(added.error());
            }
        }
        auto source = std::move(builder).finish();
        if (!source) return cxx::unexpected(source.error());
        for (const auto& pass : source->passes())
        {
            const auto* raster = std::get_if<RasterWork>(&pass.command);
            const auto* stage = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
            if (!stage) continue;
            for (const auto& input : stage->draw_resources)
            {
                auto slot = declareStorage({input.binding, input.source, input.reads, 0, input.optional});
                if (!slot) return cxx::unexpected(slot.error());
            }
        }
        std::ranges::sort(draft.target_imports, {}, &detail::TargetImageSlot::binding);
        if (auto ordered = orderStorageBindings(); !ordered) return cxx::unexpected(ordered.error());
        return source;
    }

    RenderResult<RenderGraphAnalysis>
    analyzeEffectResources(const GraphDefinition& source, EffectDeclarations& draft, rdesc::EShaderTarget target) noexcept
    {
        std::vector<bool> linked_targets(source.resources().size());
        std::vector<EffectResourceOperand*> linked_storages(source.resources().size());
        std::vector<bool> linked_buffers(source.resources().size());
        std::vector<const ConstantBufferDeclaration*> linked_uniforms(source.resources().size());
        for (const auto& uniform : draft.uniforms) linked_uniforms[uniform.resource.index] = &uniform;
        for (const auto& buffer : draft.buffers) linked_buffers[buffer.resource.index] = true;
        for (const auto& slot : draft.target_imports) linked_targets[slot.resource.index] = true;
        for (auto& storage : draft.operands)
            if (storage.slot.resource.index != kGraphInvalidIndex) linked_storages[storage.slot.resource.index] = &storage;
        for (std::size_t index = 0; index < source.resources().size(); ++index)
        {
            const auto origin = source.resources()[index].origin;
            const bool linked = linked_targets[index] || linked_storages[index] ||
                linked_uniforms[index] || linked_buffers[index];
            const bool unbound_import = origin == EGraphOrigin::IMPORTED && !linked;
            const bool unbound_frame_buffer = origin == EGraphOrigin::FRAME_BUFFER && !linked_buffers[index];
            if (unbound_import || unbound_frame_buffer)
                return cxx::unexpected(RenderError{kRuntimeUnsupported, {index}});
        }
        for (std::size_t p = 0; p < source.passes().size(); ++p)
        {
            for (const auto& use : source.passes()[p].uses)
            {
                const auto valid = [&](GraphResourceId resource) noexcept
                {
                    if (static_cast<unsigned>(use.usage) > static_cast<unsigned>(EGraphUse::UNIFORM_READ)) return false;
                    auto* declaration = linked_storages[resource.index];
                    if (!declaration) return true;
                    declaration->graph_uses |= 1u << static_cast<unsigned>(use.usage);
                    if (use.access != EGraphAccess::READ && !source.resources()[resource.index].output_range)
                        return false;
                    const auto required = std::get<ResourceProjection>(declaration->contract).read_use;
                    return !required || (use.access == EGraphAccess::READ && use.usage == *required);
                };
                const auto resource = use.version.resource;
                if (!valid(resource) || (use.fallback && !valid(use.fallback->resource)))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, resource.index}});
            }
        }
        auto analysis = analyzeFrameAlgorithms(source, draft, target);
        if (!analysis) return cxx::unexpected(analysis.error());
        for (const auto pass : analysis->execution_order)
        {
            const auto& graph_pass = source.passes()[pass.index];
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
                                parameter.uniform_constant, uniform->source.block, target
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
        return analysis;
    }
}
