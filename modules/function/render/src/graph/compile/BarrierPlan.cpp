#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <algorithm>
#include <lux/engine/render/graph/compile/BuiltinPass.hpp>
#include <tuple>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/compile/BarrierPlan.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>

namespace lux::render::detail
{
    namespace
    {
        struct Access final
        {
            nri::AccessBits access = nri::AccessBits::NONE;
            nri::StageBits stages = nri::StageBits::ALL;
            nri::Layout layout = nri::Layout::UNDEFINED;
            bool write = false;
            bool known = false;
            std::uint32_t pass_slot = kGraphInvalidIndex;
            ETransitionBoundary boundary = ETransitionBoundary::PASS;
            EQueueRole queue = EQueueRole::GRAPHICS;
            std::uint32_t resource = kGraphInvalidIndex;
        };

        Access accessFor(const ResourceAccess& use, const PassBindings& program) noexcept
        {
            Access result;
            result.write = use.access != EGraphAccess::READ;
            result.known = true;
            switch (use.usage)
            {
            case EGraphUse::COLOR_ATTACHMENT:
                result.access =
                    result.write ? nri::AccessBits::COLOR_ATTACHMENT : nri::AccessBits::COLOR_ATTACHMENT_READ;
                result.stages = nri::StageBits::COLOR_ATTACHMENT;
                result.layout = nri::Layout::COLOR_ATTACHMENT;
                break;
            case EGraphUse::DEPTH_ATTACHMENT:
                result.access = nri::AccessBits::DEPTH_STENCIL_ATTACHMENT;
                result.stages = nri::StageBits::DEPTH_STENCIL_ATTACHMENT;
                result.layout = nri::Layout::DEPTH_STENCIL_ATTACHMENT;
                break;
            case EGraphUse::TRANSFER_SOURCE:
            case EGraphUse::TRANSFER_DESTINATION:
                result.access = use.usage == EGraphUse::TRANSFER_DESTINATION ? nri::AccessBits::COPY_DESTINATION
                                                                             : nri::AccessBits::COPY_SOURCE;
                result.stages = nri::StageBits::COPY;
                result.layout = use.usage == EGraphUse::TRANSFER_DESTINATION ? nri::Layout::COPY_DESTINATION
                                                                             : nri::Layout::COPY_SOURCE;
                break;
            case EGraphUse::VERTEX:
                result.access = nri::AccessBits::VERTEX_BUFFER;
                result.stages = nri::StageBits::VERTEX_SHADER;
                break;
            case EGraphUse::INDEX:
                result.access = nri::AccessBits::INDEX_BUFFER;
                result.stages = nri::StageBits::INDEX_INPUT;
                break;
            case EGraphUse::INDIRECT:
                result.access = nri::AccessBits::ARGUMENT_BUFFER;
                result.stages = nri::StageBits::INDIRECT;
                break;
            case EGraphUse::UNIFORM_READ:
                result.access = nri::AccessBits::CONSTANT_BUFFER;
                break;
            default:
                result.access = use.usage == EGraphUse::SAMPLED ? nri::AccessBits::SHADER_RESOURCE
                                                                : nri::AccessBits::SHADER_RESOURCE_STORAGE;
                result.layout = use.usage == EGraphUse::SAMPLED ? nri::Layout::SHADER_RESOURCE
                                                                : nri::Layout::SHADER_RESOURCE_STORAGE;
                bool first_binding = true;
                for (std::size_t field = 0; field < program.uses.size(); ++field)
                {
                    if (program.uses[field].resource == use.version.resource.index)
                    {
                        const auto stages = shaderStages(program.bindings[program.uses[field].field].stages);
                        result.stages = first_binding ? stages : combineStages(result.stages, stages);
                        if (first_binding) result.access = nri::AccessBits::NONE;
                        first_binding = false;
                        if (program.bindings[program.uses[field].field].kind == EDescriptorKind::UNIFORM_BUFFER)
                        {
                            result.access |= nri::AccessBits::CONSTANT_BUFFER;
                        }
                        else
                        {
                            const auto& binding = program.bindings[program.uses[field].field];
                            const bool is_read_only_buffer = binding.kind == EDescriptorKind::STORAGE_BUFFER &&
                                binding.access == EShaderAccess::READ_ONLY;
                            const bool is_shader_resource = use.usage == EGraphUse::SAMPLED || is_read_only_buffer;
                            result.access |= is_shader_resource ? nri::AccessBits::SHADER_RESOURCE :
                                nri::AccessBits::SHADER_RESOURCE_STORAGE;
                        }
                    }
                }
                break;
            }
            return result;
        }

        struct PassAccessContext final
        {
            const PassBindings& program;
            const GraphPass& pass;
            nri::GraphicsAPI backend;
        };

        Access
        accessForUse(
            const ResourceAccess& use,
            const PassAccessContext& context,
            const VGraphResourceDesc& description
        ) noexcept
        {
            const auto& [program, pass, backend] = context;
            auto current = accessFor(use, program);
            const auto* raster = std::get_if<RasterWork>(&pass.command);
            const auto* stream = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
            const bool is_stream_buffer = stream && stream->usesStreamProducer() &&
                use.usage == EGraphUse::STORAGE_READ && std::holds_alternative<GraphBufferDesc>(description);
            const bool is_stream_image = stream && stream->usesStreamProducer() &&
                use.usage == EGraphUse::SAMPLED && std::holds_alternative<GraphImageDesc>(description);
            if (is_stream_buffer || is_stream_image)
            {
                auto access = nri::AccessBits::NONE;
                auto stages = nri::StageBits::ALL;
                for (const auto& input : stream->inputs)
                {
                    if (input.usage != use.usage || !input.kind) continue;
                    if (is_stream_image && *input.kind != rdesc::EDescriptorType::SAMPLED_IMAGE) continue;
                    const auto selected = shaderStages(input.stages);
                    stages = access == nri::AccessBits::NONE ? selected : combineStages(stages, selected);
                    access |= *input.kind == rdesc::EDescriptorType::UNIFORM_BUFFER
                        ? nri::AccessBits::CONSTANT_BUFFER : nri::AccessBits::SHADER_RESOURCE;
                }
                if (access != nri::AccessBits::NONE)
                {
                    current.access = access;
                    current.stages = stages;
                }
            }
            if (const auto* transfer = std::get_if<GraphTransferCommand>(&pass.command))
            {
                const auto native = TransferPass::access(
                    transfer->work, transfer->typed_buffer_writes, use, description, backend
                );
                if (native)
                {
                    current.access = native->access;
                    current.stages = native->stages;
                    if (std::holds_alternative<GraphImageDesc>(description)) current.layout = native->layout;
                }
            }
            return current;
        }

        void includeUsage(CompiledResource& resource, const Access& access) noexcept
        {
            if (std::holds_alternative<GraphImageDesc>(resource.description))
            {
                switch (access.layout)
                {
                case nri::Layout::COLOR_ATTACHMENT:
                    resource.image_usage |= nri::TextureUsageBits::COLOR_ATTACHMENT;
                    break;
                case nri::Layout::DEPTH_STENCIL_ATTACHMENT:
                    resource.image_usage |= nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
                    break;
                case nri::Layout::SHADER_RESOURCE:
                    resource.image_usage |= nri::TextureUsageBits::SHADER_RESOURCE;
                    break;
                case nri::Layout::SHADER_RESOURCE_STORAGE:
                    resource.image_usage |= nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
                    break;
                default:
                    break;
                }
            }
            else
            {
                const auto include = [&](nri::AccessBits bit, nri::BufferUsageBits usage)
                {
                    if (access.access & bit)
                    {
                        resource.buffer_usage |= usage;
                    }
                };
                include(nri::AccessBits::VERTEX_BUFFER, nri::BufferUsageBits::VERTEX);
                include(nri::AccessBits::INDEX_BUFFER, nri::BufferUsageBits::INDEX);
                include(nri::AccessBits::ARGUMENT_BUFFER, nri::BufferUsageBits::ARGUMENT);
                include(nri::AccessBits::CONSTANT_BUFFER, nri::BufferUsageBits::CONSTANT);
                include(nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
                include(nri::AccessBits::SHADER_RESOURCE, nri::BufferUsageBits::SHADER_RESOURCE);
                include(nri::AccessBits::CLEAR_STORAGE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
            }
        }

        Access externalState(GraphResourceId resource, EGraphUse usage) noexcept
        {
            ResourceAccess use{{resource, 0}};
            use.usage = usage;
            const bool writes = usage == EGraphUse::TRANSFER_DESTINATION || usage == EGraphUse::STORAGE_WRITE ||
                                usage == EGraphUse::STORAGE_READ_WRITE || usage == EGraphUse::COLOR_ATTACHMENT ||
                                usage == EGraphUse::DEPTH_ATTACHMENT;
            use.access = writes ? EGraphAccess::WRITE : EGraphAccess::READ;
            return accessFor(use, {});
        }
    } // namespace

    void GraphCompiler::Workspace::collectUseStages() noexcept
    {
        const auto& graph = source.graph;
        const auto order = std::span(source.analysis.execution_order);
        const auto backend = device.limits.graphicsAPI;
        use_stages.resize(graph.passes().size());
        for (std::size_t slot = 0; slot < order.size(); ++slot)
        {
            const auto id = order[slot].index;
            const auto& pass = graph.passes()[id];
            const PassAccessContext context{bindings.passes[slot], pass, backend};
            auto& uses = use_stages[id];
            uses.clear();
            uses.reserve(pass.uses.size());
            for (const auto& use : pass.uses)
            {
                const auto fallback = use.access == EGraphAccess::READ && use.fallback
                    ? use.fallback->resource.index : kGraphInvalidIndex;
                for (auto resource : {use.version.resource.index, fallback})
                {
                    if (resource != kGraphInvalidIndex)
                    {
                        const auto access = accessForUse(use, context, graph.resources()[resource].description);
                        if (allocation == ETransientAllocation::ALIAS_MEMORY)
                        {
                            const auto p = static_cast<std::uint32_t>(slot);
                            alias_uses.push_back({
                                {resource}, {use.range, {access.access, access.stages}, access.write, p, p}
                            });
                        }
                        uses.emplace_back(resource, access.stages);
                    }
                }
            }
            std::ranges::sort(uses, {}, &ResourceUseStages::resource_index);
            std::size_t count = 0;
            for (const auto& use : uses)
            {
                if (count && uses[count - 1].resource_index == use.resource_index)
                {
                    uses[count - 1].stages = combineStages(uses[count - 1].stages, use.stages);
                }
                else
                {
                    uses[count++] = use;
                }
            }
            uses.resize(count);
        }
    }

    void compactImageTransitions(std::span<CompiledResource> resources, RecordingProgram& recording) noexcept
    {
        const auto key = [](const ImageTransition& transition) noexcept
        {
            const auto& barrier = transition.native;
            return std::tuple{
                transition.resource, transition.previous_slot, transition.boundary, barrier.planes,
                barrier.before.access, barrier.before.layout, barrier.before.stages,
                barrier.after.access, barrier.after.layout, barrier.after.stages
            };
        };
        const auto compact = [&](BarrierBatch& batch) noexcept
        {
            for (bool layers : {false, true})
            {
                std::ranges::sort(batch.images, [&](const auto& left, const auto& right) noexcept
                {
                    const auto a = key(left), b = key(right);
                    if (a != b) return a < b;
                    const auto& x = left.native;
                    const auto& y = right.native;
                    return layers ? std::tie(x.mipOffset, x.mipNum, x.layerOffset) <
                                        std::tie(y.mipOffset, y.mipNum, y.layerOffset)
                                  : std::tie(x.layerOffset, x.layerNum, x.mipOffset) <
                                        std::tie(y.layerOffset, y.layerNum, y.mipOffset);
                });
                std::size_t count = 0;
                for (const auto& transition : batch.images)
                {
                    bool merged = false;
                    if (count && key(batch.images[count - 1]) == key(transition))
                    {
                        auto& previous = batch.images[count - 1].native;
                        const auto& next = transition.native;
                        const bool same_identity = previous.texture == next.texture &&
                            previous.srcQueue == next.srcQueue && previous.dstQueue == next.dstQueue;
                        const bool adjacent = layers
                            ? previous.mipOffset == next.mipOffset && previous.mipNum == next.mipNum &&
                              std::uint32_t(previous.layerOffset) + previous.layerNum == next.layerOffset
                            : previous.layerOffset == next.layerOffset && previous.layerNum == next.layerNum &&
                              std::uint32_t(previous.mipOffset) + previous.mipNum == next.mipOffset;
                        if (same_identity && adjacent)
                        {
                            if (layers) previous.layerNum += next.layerNum;
                            else previous.mipNum += next.mipNum;
                            merged = true;
                        }
                    }
                    if (!merged) batch.images[count++] = transition;
                }
                batch.images.resize(count);
            }
        };
        for (auto& resource : resources)
        {
            resource.barrier_uses.clear();
            resource.final_barrier_uses.clear();
        }
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& batch = recording.passes[p].before;
            compact(batch);
            for (std::uint32_t i = 0; i < batch.images.size(); ++i)
                resources[batch.images[i].resource].barrier_uses.emplace_back(p, i);
            for (std::uint32_t i = 0; i < batch.buffers.size(); ++i)
                resources[batch.buffers[i].resource].barrier_uses.emplace_back(p, i);
        }
        compact(recording.after);
        for (std::uint32_t i = 0; i < recording.after.images.size(); ++i)
            resources[recording.after.images[i].resource].final_barrier_uses.push_back(i);
        for (std::uint32_t i = 0; i < recording.after.buffers.size(); ++i)
            resources[recording.after.buffers[i].resource].final_barrier_uses.push_back(i);
    }

    RenderResult<void>
    GraphCompiler::Workspace::planBarriers(
        compiled::RenderGraph& plan, const PhysicalResourcePlan* physical
    ) const noexcept
    {
        const auto& graph = source.graph;
        const auto& analysis = source.analysis;
        const auto& layouts = bindings;
        auto& resources = plan.resources_;
        auto& recording = plan.recording_;
        const auto& target = plan.output_description_;
        const auto backend = device.limits.graphicsAPI;
        const bool legacy_d3d12 = backend == nri::GraphicsAPI::D3D12 && !device.limits.features.enhancedBarriers;
        // Image cells extend that algorithm to actual aspects/mips/layers; NRI Buffer barriers are whole-resource.
        std::vector<std::vector<Access>> states(resources.size());
        recording.after = {};
        for (auto& pass : recording.passes)
        {
            pass.before = {};
        }
        for (std::size_t i = 0; i < resources.size(); ++i)
        {
            resources[i].descriptor_uses.clear();
            resources[i].final_barrier_uses.clear();
            const auto* image = std::get_if<GraphImageDesc>(&resources[i].description);
            states[i].resize(image ? std::size_t(image->mips) * image->layers * 3 : 1);
            if (resources[i].memory_used && resources[i].origin != EGraphOrigin::TRANSIENT &&
                !resources[i].shared_table)
            {
                const auto usage = graph.resources()[i].initial_use;
                if (image && usage == EGraphUse::UNSPECIFIED && !graph.resources()[i].initial_ranges.empty())
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                if (usage != EGraphUse::UNSPECIFIED)
                {
                    includeUsage(resources[i], externalState(GraphResourceId{static_cast<std::uint32_t>(i)}, usage));
                    for (std::size_t cell = 0; cell < states[i].size(); ++cell)
                    {
                        const auto plane = image ? cell / (image->mips * image->layers) : 0;
                        if (!image || (rdesc::textureAspectMask(image->format) & (1u << plane)))
                        {
                            states[i][cell] = externalState(GraphResourceId{static_cast<std::uint32_t>(i)}, usage);
                            states[i][cell].resource = static_cast<std::uint32_t>(i);
                        }
                    }
                }
            }
        }
        auto apply = [&](std::uint32_t index, std::size_t cell, Access current, BarrierBatch& batch)
        {
            const auto slot = physical ? physical->resource_slots[index] : PhysicalResourceSlotIndex{};
            const auto identity = slot.valid() ? physical->slots[slot.index].resource.index : index;
            auto& previous = states[identity][cell];
            current.resource = index;
            const bool cross_queue = previous.queue != current.queue;
            const auto* image = std::get_if<GraphImageDesc>(&resources[index].description);
            // Read-after-read can still need visibility established by an earlier write.
            // Only reuse a state whose access and execution scopes already cover this read.
            const bool covers_read = coversScope({previous.access, previous.stages}, {current.access, current.stages});
            const bool merge_reads = covers_read && previous.known &&
                                     previous.resource == index && !cross_queue &&
                                     !previous.write && !current.write &&
                                     previous.pass_slot != kGraphInvalidIndex &&
                                     current.boundary == ETransitionBoundary::PASS &&
                                     (!image || previous.layout == current.layout);
            const bool transition = !merge_reads;
            if (transition)
            {
                if (image)
                {
                    nri::TextureBarrierDesc barrier{};
                    // With legacy barriers, the access bits are the native resource state.
                    // The graphics export epilogue waits on the producer before restoring COMMON.
                    const bool restores_legacy_output = legacy_d3d12 && index == plan.output_ &&
                        current.boundary == ETransitionBoundary::EXPORT && current.layout == nri::Layout::GENERAL;
                    barrier.before = {
                        cross_queue && !restores_legacy_output ? nri::AccessBits::NONE : previous.access,
                        previous.layout,
                        cross_queue ? nri::StageBits::ALL : previous.stages
                    };
                    barrier.after = {current.access, current.layout, current.stages};
                    barrier.mipOffset = static_cast<nri::Dim_t>(cell % image->mips);
                    barrier.mipNum = 1;
                    barrier.layerOffset = static_cast<nri::Dim_t>((cell / image->mips) % image->layers);
                    barrier.layerNum = 1;
                    const auto plane = cell / (image->mips * image->layers);
                    barrier.planes = plane == 0   ? nri::PlaneBits::COLOR
                                     : plane == 1 ? nri::PlaneBits::DEPTH
                                                  : nri::PlaneBits::STENCIL;
                    batch.images.push_back({index, barrier, previous.pass_slot, current.boundary});
                }
                else
                {
                    nri::BufferBarrierDesc barrier{};
                    barrier.before = {
                        cross_queue ? nri::AccessBits::NONE : previous.access,
                        cross_queue ? nri::StageBits::ALL : previous.stages
                    };
                    barrier.after = {current.access, current.stages};
                    const bool bind_initial_access = current.boundary == ETransitionBoundary::PASS &&
                        current.queue == EQueueRole::GRAPHICS && previous.pass_slot == kGraphInvalidIndex &&
                        barrier.before.stages == nri::StageBits::ALL && barrier.before.access == nri::AccessBits::NONE;
                    batch.buffers.push_back({
                        index, barrier, previous.pass_slot, current.boundary, bind_initial_access
                    });
                }
            }
            else if (!cross_queue)
            {
                current.stages = combineStages(current.stages, previous.stages);
                current.access |= previous.access;
            }
            previous = current;
        };
        auto cells = [&](std::uint32_t index, const VGraphRange& range, auto&& visit)
        {
            if (const auto* image = std::get_if<GraphImageDesc>(&resources[index].description))
            {
                const auto& selected = std::get<GraphImageRange>(range);
                for (std::uint32_t plane = 0; plane < 3; ++plane)
                {
                    if (!(selected.aspects & (1u << plane)))
                    {
                        continue;
                    }
                    for (std::uint32_t layer = selected.first_layer;
                         layer < selected.first_layer + selected.layer_count;
                         ++layer)
                    {
                        for (std::uint32_t mip = selected.first_mip; mip < selected.first_mip + selected.mip_count;
                             ++mip)
                        {
                            visit((plane * image->layers + layer) * image->mips + mip);
                        }
                    }
                }
            }
            else
            {
                visit(0u);
            }
        };
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            const auto& declared = graph.passes()[analysis.execution_order[p].index];
            const auto& layout = layouts.passes[pass.binding_slot];
            std::vector<std::vector<Access>> next(resources.size());
            std::vector<std::uint32_t> touched;
            bool incompatible_layouts = false;
            for (const auto& use : declared.uses)
            {
                if (use.usage == EGraphUse::UNSPECIFIED)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                const auto fallback = use.access == EGraphAccess::READ && use.fallback ? use.fallback->resource.index
                                                                                       : kGraphInvalidIndex;
                // Both possible inputs have a known state. Runtime selects the physical read,
                // not a different barrier plan or an extra copy of the fallback contents.
                for (const auto index : {use.version.resource.index, fallback})
                {
                    if (index == kGraphInvalidIndex)
                    {
                        continue;
                    }
                    auto& resource = resources[index];
                    if (index == fallback && resource.history != kGraphInvalidIndex)
                    {
                        return cxx::unexpected(RenderError{kDeviceUnsupported});
                    }
                    if (resource.shared_table)
                    {
                        continue;
                    }
                    auto current = accessForUse(use, {layout, declared, backend}, resource.description);
                    current.queue = pass.queue;
                    current.pass_slot = p;
                    includeUsage(resource, current);
                    if (next[index].empty())
                    {
                        next[index].resize(states[index].size());
                        touched.push_back(index);
                    }
                    cells(
                        index,
                        use.range,
                        [&](auto cell)
                        {
                            auto& combined = next[index][cell];
                            if (!combined.known)
                            {
                                combined = current;
                            }
                            else
                            {
                                incompatible_layouts |= std::holds_alternative<GraphImageDesc>(resource.description) &&
                                                        combined.layout != current.layout;
                                combined.access |= current.access;
                                combined.stages = combineStages(combined.stages, current.stages);
                                combined.write |= current.write;
                            }
                        }
                    );
                }
            }
            if (incompatible_layouts)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            for (auto index : touched)
            {
                for (std::size_t cell = 0; cell < next[index].size(); ++cell)
                {
                    if (next[index][cell].known)
                    {
                        apply(index, cell, next[index][cell], pass.before);
                    }
                }
            }
            for (std::uint32_t binding = 0; binding < layout.uses.size(); ++binding)
            {
                if (layout.uses[binding].resource != kGraphInvalidIndex &&
                    layout.bindings[layout.uses[binding].field].count != 0)
                {
                    resources[layout.uses[binding].resource].descriptor_uses.emplace_back(p, binding);
                    if (layout.uses[binding].fallback != kGraphInvalidIndex)
                    {
                        resources[layout.uses[binding].fallback].descriptor_uses.emplace_back(p, binding);
                    }
                }
            }
        }
        std::vector<std::vector<Access>> final_states(resources.size());
        for (std::uint32_t i = 0; i < resources.size(); ++i)
        {
            const auto usage = graph.resources()[i].initial_use;
            if (!resources[i].memory_used || resources[i].shared_table ||
                resources[i].origin == EGraphOrigin::TRANSIENT || usage == EGraphUse::UNSPECIFIED)
            {
                continue;
            }
            final_states[i].resize(states[i].size());
            for (std::size_t cell = 0; cell < states[i].size(); ++cell)
            {
                if (states[i][cell].pass_slot != kGraphInvalidIndex)
                {
                    final_states[i][cell] = externalState(GraphResourceId{i}, usage);
                    final_states[i][cell].boundary = ETransitionBoundary::RESTORE_IMPORTED;
                }
            }
        }
        for (const auto& output : graph.outputs())
        {
            const bool presented = output.target && target.kind == ETargetKind::SURFACE;
            const bool sampled_target = output.target && target.sampled;
            Access state{.known = true};
            if (presented)
            {
                if (output.final_use)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                state.stages = nri::StageBits::NONE;
                state.layout = nri::Layout::PRESENT;
            }
            else if (output.final_use || sampled_target)
            {
                const auto use = output.final_use.value_or(EGraphUse::SAMPLED);
                if (sampled_target && use != EGraphUse::SAMPLED)
                    return cxx::unexpected(RenderError{kDeviceArgument});
                state = externalState(output.version.resource, use);
            }
            else if (legacy_d3d12 && output.target)
            {
                // Legacy textures retain RENDER_TARGET across submissions. Close the cycle at
                // COMMON, the native before-state of the next recording's first target transition.
                state.layout = nri::Layout::GENERAL;
            }
            else continue;
            state.boundary = ETransitionBoundary::EXPORT;
            const auto close = [&](GraphResourceId resource)
            {
                const auto index = resource.index;
                includeUsage(resources[index], state);
                if (final_states[index].empty()) final_states[index].resize(states[index].size());
                cells(index, output.range, [&](auto cell) { final_states[index][cell] = state; });
            };
            close(output.version.resource);
            if (output.alternate && output.alternate->value.resource != output.version.resource)
                close(output.alternate->value.resource);
        }
        for (std::uint32_t i = 0; i < final_states.size(); ++i)
        {
            for (std::size_t cell = 0; cell < final_states[i].size(); ++cell)
            {
                if (final_states[i][cell].known)
                {
                    apply(i, cell, final_states[i][cell], recording.after);
                }
            }
        }
        return {};
    }
} // namespace lux::render::detail
