#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/BarrierPlan.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
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
            GraphPassId pass;
            ETransitionBoundary boundary = ETransitionBoundary::PASS;
            EQueueRole queue = EQueueRole::GRAPHICS;
            std::uint32_t resource = kGraphInvalidIndex;
        };

        nri::StageBits combineStages(nri::StageBits a, nri::StageBits b) noexcept
        {
            // NRI encodes ALL as zero, so a bitwise OR would narrow an already established ALL scope.
            return a == nri::StageBits::ALL || b == nri::StageBits::ALL ? nri::StageBits::ALL : a | b;
        }

        Access accessFor(const ResourceAccess& use, const PassLayout& program) noexcept
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
            default:
                result.access = use.usage == EGraphUse::SAMPLED ? nri::AccessBits::SHADER_RESOURCE
                                                                : nri::AccessBits::SHADER_RESOURCE_STORAGE;
                result.layout = use.usage == EGraphUse::SAMPLED ? nri::Layout::SHADER_RESOURCE
                                                                : nri::Layout::SHADER_RESOURCE_STORAGE;
                if (!program.uses.empty())
                {
                    result.access = nri::AccessBits::NONE;
                }
                for (std::size_t field = 0; field < program.uses.size(); ++field)
                {
                    if (program.uses[field].resource == use.version.resource.index)
                    {
                        result.stages |= shaderStages(program.bindings[program.uses[field].field].stages);
                        if (program.bindings[program.uses[field].field].kind == EDescriptorKind::UNIFORM_BUFFER)
                        {
                            result.access |= nri::AccessBits::CONSTANT_BUFFER;
                        }
                        else
                        {
                            result.access |= use.usage == EGraphUse::SAMPLED ? nri::AccessBits::SHADER_RESOURCE
                                                                             : nri::AccessBits::SHADER_RESOURCE_STORAGE;
                        }
                    }
                }
                break;
            }
            return result;
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
                include(nri::AccessBits::VERTEX_BUFFER, nri::BufferUsageBits::VERTEX_BUFFER);
                include(nri::AccessBits::INDEX_BUFFER, nri::BufferUsageBits::INDEX_BUFFER);
                include(nri::AccessBits::ARGUMENT_BUFFER, nri::BufferUsageBits::ARGUMENT_BUFFER);
                include(nri::AccessBits::CONSTANT_BUFFER, nri::BufferUsageBits::CONSTANT_BUFFER);
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

    RenderResult<void> planBarriers(
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        const PipelineLayoutPlan& layouts,
        std::span<CompiledResource> resources,
        RecordingProgram& recording,
        const PhysicalResourcePlan* physical
    ) noexcept
    {
        // Legacy computeBarriers/buildPrebuiltBarriers: derive states once, record indexed templates.
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
            const auto slot = physical ? physical->resource_slots[index] : kGraphInvalidIndex;
            const auto identity = slot == kGraphInvalidIndex ? index : physical->slots[slot].index;
            auto& previous = states[identity][cell];
            current.resource = index;
            const bool cross_queue = previous.queue != current.queue;
            const auto* image = std::get_if<GraphImageDesc>(&resources[index].description);
            const bool transition = !previous.known || previous.resource != index ||
                                    (image && previous.layout != current.layout) || previous.write || current.write ||
                                    previous.access != current.access;
            if (transition)
            {
                if (image)
                {
                    nri::TextureBarrierDesc barrier{};
                    barrier.before = {
                        cross_queue ? nri::AccessBits::NONE : previous.access,
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
                    batch.images.push_back({index, barrier, previous.pass, current.boundary});
                }
                else
                {
                    nri::BufferBarrierDesc barrier{};
                    barrier.before = {
                        cross_queue ? nri::AccessBits::NONE : previous.access,
                        cross_queue ? nri::StageBits::ALL : previous.stages
                    };
                    barrier.after = {current.access, current.stages};
                    batch.buffers.push_back({index, barrier, previous.pass, current.boundary});
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
            const auto& layout = layouts.passes[pass.source_pass];
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
                    auto current = accessFor(use, layout);
                    current.queue = pass.queue;
                    current.pass = analysis.execution_order[p];
                    if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
                    {
                        if (const auto* transfer = std::get_if<TransferWork>(&declared.work))
                        {
                            for (const auto& operation : transfer->operations)
                            {
                                const auto* clear = std::get_if<ClearImage>(&operation);
                                if (clear && clear->destination.version.resource.index == index)
                                {
                                    const bool depth = rdesc::textureAspectMask(image->format) != 1;
                                    current.access = depth ? nri::AccessBits::DEPTH_STENCIL_ATTACHMENT
                                                           : nri::AccessBits::COLOR_ATTACHMENT;
                                    current.stages = depth ? nri::StageBits::DEPTH_STENCIL_ATTACHMENT
                                                           : nri::StageBits::COLOR_ATTACHMENT;
                                    current.layout =
                                        depth ? nri::Layout::DEPTH_STENCIL_ATTACHMENT : nri::Layout::COLOR_ATTACHMENT;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (const auto* transfer = std::get_if<TransferWork>(&declared.work))
                        {
                            for (const auto& operation : transfer->operations)
                            {
                                const auto* fill = std::get_if<FillBuffer>(&operation);
                                if (fill && fill->destination.version.resource.index == index)
                                {
                                    // NRI v180 implements ZeroBuffer with vkCmdFillBuffer on Vulkan.
                                    // Cover both the API's copy operation and its native clear stage.
                                    current.stages = nri::StageBits::COPY | nri::StageBits::CLEAR_STORAGE;
                                    if (fill->value)
                                    {
                                        current.access = nri::AccessBits::CLEAR_STORAGE;
                                        current.stages = nri::StageBits::CLEAR_STORAGE;
                                        resource.buffer_usage |= nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
                                    }
                                }
                            }
                        }
                    }
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
                if (states[i][cell].pass.index != kGraphInvalidIndex)
                {
                    final_states[i][cell] = externalState(GraphResourceId{i}, usage);
                    final_states[i][cell].boundary = ETransitionBoundary::RESTORE_IMPORTED;
                }
            }
        }
        for (const auto& output : graph.outputs())
        {
            if (!output.final_use)
            {
                continue;
            }
            const auto index = output.version.resource.index;
            auto state = externalState(output.version.resource, *output.final_use);
            state.boundary = ETransitionBoundary::EXPORT;
            includeUsage(resources[index], state);
            if (final_states[index].empty())
            {
                final_states[index].resize(states[index].size());
            }
            cells(index, output.range, [&](auto cell) { final_states[index][cell] = state; });
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
        for (std::uint32_t i = 0; i < recording.after.images.size(); ++i)
        {
            resources[recording.after.images[i].resource].final_barrier_uses.push_back(i);
        }
        for (std::uint32_t i = 0; i < recording.after.buffers.size(); ++i)
        {
            resources[recording.after.buffers[i].resource].final_barrier_uses.push_back(i);
        }
        return {};
    }
} // namespace lux::render::detail
