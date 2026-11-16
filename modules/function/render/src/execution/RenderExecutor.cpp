#include <lux/engine/render/device/Error.hpp>
#include <algorithm>
#include <cstring>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/BindingState.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/HistoryState.hpp>
#include <lux/engine/render/execution/RenderExecutor.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    namespace
    {
        void bindDescriptors(
            DeviceSession& session,
            nri::CommandBuffer& command,
            const PassBindings& bindings,
            std::span<nri::DescriptorSet* const> private_sets,
            BindingState& state
        ) noexcept
        {
            const auto bind = [&](std::uint32_t set, nri::DescriptorSet* descriptors) noexcept
            {
                if (state.descriptors[set] != descriptors)
                {
                    session.nriCore().CmdSetDescriptorSet(command, {set, descriptors, state.point});
                    state.descriptors[set] = descriptors;
                }
            };
            constexpr auto kinds = std::array{
                nri::DescriptorType::TEXTURE,
                nri::DescriptorType::SAMPLER,
                nri::DescriptorType::STRUCTURED_BUFFER
            };
            for (std::size_t kind = 0; kind < kinds.size(); ++kind)
            {
                const auto set = bindings.shared_sets[kind];
                if (set != kGraphInvalidIndex)
                {
                    auto* descriptors = session.description().graphicsAPI == nri::GraphicsAPI::D3D12 ?
                        state.shared_descriptors[kind] : session.descriptors().descriptors(kinds[kind]);
                    bind(set, descriptors);
                }
            }
            for (std::uint32_t i = 0; i < private_sets.size(); ++i) bind(bindings.private_set + i, private_sets[i]);
        }

        void
        setScissor(
            const nri::CoreInterface& core,
            nri::CommandBuffer& command,
            const nri::Rect& value,
            BindingState& state
        ) noexcept
        {
            const bool changed = !state.scissor || state.scissor->x != value.x || state.scissor->y != value.y ||
                                 state.scissor->width != value.width || state.scissor->height != value.height;
            if (changed)
            {
                core.CmdSetScissors(command, &value, 1);
                state.scissor = value;
            }
        }

        void
        setDepthBias(
            const nri::CoreInterface& core,
            nri::CommandBuffer& command,
            const nri::DepthBiasDesc& value,
            BindingState& state
        ) noexcept
        {
            const bool changed = !state.depth_bias || state.depth_bias->constant != value.constant ||
                                 state.depth_bias->clamp != value.clamp || state.depth_bias->slope != value.slope;
            if (changed)
            {
                core.CmdSetDepthBias(command, value);
                state.depth_bias = value;
            }
        }
    } // namespace

    void RenderExecutor::recordRasterDraws(
        DeviceSession& session,
        nri::CommandBuffer& command,
        const RasterDrawList& list,
        BindingState& state
    ) noexcept
    {
        const auto& core = session.nriCore();
        // Graph-local vertex indices cannot describe the physical arrays in a dynamic packet.
        state.vertices = {};
        std::span<const RasterDrawList::Vertex> previous_vertices;
        for (const auto& section : list.sections)
        {
            const auto colors = std::span(list.colors).subspan(section.first_color, section.color_count);
            nri::RenderingDesc rendering{};
            rendering.colors = colors.data();
            rendering.colorNum = static_cast<std::uint32_t>(colors.size());
            rendering.depth = section.depth;
            core.CmdBeginRendering(command, rendering);
            core.CmdSetViewports(command, &section.viewport, 1);
            bool section_parameters_pending = section.parameter_size != 0;
            for (const auto& packet : std::span(list.packets).subspan(section.first_draw, section.draw_count))
            {
                if (state.layout != packet.bindings->layout || state.point != nri::BindPoint::GRAPHICS)
                {
                    core.CmdSetPipelineLayout(command, nri::BindPoint::GRAPHICS, *packet.bindings->layout);
                    state.layout = packet.bindings->layout;
                    state.point = nri::BindPoint::GRAPHICS;
                    state.descriptors = {};
                    section_parameters_pending = section.parameter_size != 0;
                }
                if (section_parameters_pending)
                {
                    core.CmdSetRootConstants(
                        command,
                        {0,
                         list.parameters.data() + section.parameter_offset,
                         section.parameter_size,
                         0,
                         nri::BindPoint::GRAPHICS}
                    );
                    section_parameters_pending = false;
                }
                if (state.pipeline != packet.pipeline)
                {
                    core.CmdSetPipeline(command, *packet.pipeline);
                    state.pipeline = packet.pipeline;
                    state.depth_bias.reset();
                }
                if (section.depth_bias)
                {
                    setDepthBias(core, command, *section.depth_bias, state);
                }
                const auto descriptors =
                    std::span(list.descriptors).subspan(packet.first_descriptor, packet.descriptor_count);
                bindDescriptors(session, command, *packet.bindings, descriptors, state);
                const auto vertices = std::span(list.vertices).subspan(packet.first_vertex, packet.vertex_count);
                const bool same_vertices = std::ranges::equal(
                    vertices,
                    previous_vertices,
                    [](const auto& a, const auto& b) noexcept
                    {
                        return a.binding == b.binding && a.buffer.buffer == b.buffer.buffer &&
                               a.buffer.offset == b.buffer.offset && a.buffer.stride == b.buffer.stride;
                    }
                );
                if (!same_vertices)
                {
                    for (const auto& vertex : vertices)
                    {
                        core.CmdSetVertexBuffers(command, vertex.binding, &vertex.buffer, 1);
                    }
                    previous_vertices = vertices;
                }
                if (packet.parameter_size)
                {
                    core.CmdSetRootConstants(
                        command,
                        {0,
                         list.parameters.data() + packet.parameter_offset,
                         packet.parameter_size,
                         packet.parameter_destination,
                         nri::BindPoint::GRAPHICS}
                    );
                }
                setScissor(core, command, packet.scissor, state);
                std::visit(
                    [&](const auto& draw) noexcept
                    {
                        using Draw = std::remove_cvref_t<decltype(draw)>;
                        if constexpr (std::same_as<Draw, nri::DrawDesc>)
                        {
                            core.CmdDraw(command, draw);
                        }
                        else
                        {
                            const bool changed_index = state.index != packet.index ||
                                                       state.index_offset != packet.index_offset ||
                                                       state.index_type != packet.index_type;
                            if (changed_index)
                            {
                                core.CmdSetIndexBuffer(command, *packet.index, packet.index_offset, packet.index_type);
                                state.index = packet.index;
                                state.index_offset = packet.index_offset;
                                state.index_type = packet.index_type;
                            }
                            if constexpr (std::same_as<Draw, nri::DrawIndexedDesc>)
                            {
                                core.CmdDrawIndexed(command, draw);
                            }
                            else
                            {
                                core.CmdDrawIndexedIndirect(
                                    command,
                                    *draw.arguments,
                                    draw.offset,
                                    draw.count,
                                    draw.stride,
                                    draw.count_buffer,
                                    draw.count_offset
                                );
                            }
                        }
                    },
                    packet.draw
                );
            }
            core.CmdEndRendering(command);
        }
    }

    void
    RenderExecutor::recordHistoryInitialization(RenderGraphInstance& instance, nri::CommandBuffer& command) noexcept
    {
        const auto& core = instance.session_->nriCore();
        for (auto& history : instance.histories_)
        {
            const auto& description = history.plan->description;
            const auto canonical_layout = description.canonical_use == EGraphUse::SAMPLED ? nri::Layout::SHADER_RESOURCE
                                          : description.canonical_use == EGraphUse::TRANSFER_SOURCE
                                              ? nri::Layout::COPY_SOURCE
                                              : nri::Layout::SHADER_RESOURCE_STORAGE;
            const auto canonical_access =
                description.canonical_use == EGraphUse::SAMPLED           ? nri::AccessBits::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::AccessBits::COPY_SOURCE
                                                                          : nri::AccessBits::SHADER_RESOURCE_STORAGE;
            for (const auto [index, clear] :
                 {std::pair{history.previous, history.clear_previous}, std::pair{history.current, history.clear_current}
                 })
            {
                if (!clear)
                {
                    continue;
                }
                auto& slot = history.slots[index];
                if (slot.image)
                {
                    const auto& image = core.GetTextureDesc(*slot.image);
                    const auto& graph_image =
                        std::get<GraphImageDesc>(instance.plan_->resources()[description.current.index].description);
                    const auto aspects = rdesc::textureAspectMask(graph_image.format);
                    const bool depth = aspects != 1;
                    nri::TextureBarrierDesc transition{};
                    transition.texture = slot.image;
                    if (slot.initialized)
                    {
                        transition.before = {canonical_access, canonical_layout, nri::StageBits::ALL};
                    }
                    transition.after =
                        depth
                            ? nri::AccessLayoutStage{
                                  nri::AccessBits::DEPTH_STENCIL_ATTACHMENT,
                                  nri::Layout::DEPTH_STENCIL_ATTACHMENT,
                                  nri::StageBits::DEPTH_STENCIL_ATTACHMENT
                              }
                            : nri::AccessLayoutStage{
                                  nri::AccessBits::COLOR_ATTACHMENT,
                                  nri::Layout::COLOR_ATTACHMENT,
                                  nri::StageBits::COLOR_ATTACHMENT
                              };
                    transition.mipNum = image.mipNum;
                    transition.layerNum = image.layerNum;
                    transition.planes = depth ? nri::PlaneBits::DEPTH : nri::PlaneBits::COLOR;
                    if (aspects & 4)
                    {
                        transition.planes |= nri::PlaneBits::STENCIL;
                    }
                    const nri::BarrierDesc barrier{nullptr, 0, nullptr, 0, &transition, 1};
                    core.CmdBarrier(command, barrier);
                    for (auto* view : slot.initial_views)
                    {
                        nri::AttachmentDesc attachment{};
                        attachment.descriptor = view;
                        attachment.loadOp = nri::LoadOp::CLEAR;
                        attachment.storeOp = nri::StoreOp::STORE;
                        nri::RenderingDesc rendering{};
                        if (depth)
                        {
                            attachment.clearValue.depthStencil.depth = description.initial.depth;
                            rendering.depth = attachment;
                        }
                        else
                        {
                            const auto& value = description.initial.color;
                            attachment.clearValue.color.f = {value[0], value[1], value[2], value[3]};
                            rendering.colors = &attachment;
                            rendering.colorNum = 1;
                        }
                        core.CmdBeginRendering(command, rendering);
                        core.CmdEndRendering(command);
                    }
                    transition.before = transition.after;
                    transition.after = {canonical_access, canonical_layout, nri::StageBits::ALL};
                    core.CmdBarrier(command, barrier);
                }
                else
                {
                    nri::BufferBarrierDesc transition{};
                    transition.buffer = slot.buffer;
                    if (slot.initialized)
                    {
                        transition.before = {canonical_access, nri::StageBits::ALL};
                    }
                    transition.after = {
                        nri::AccessBits::COPY_DESTINATION,
                        zeroBufferStages(instance.session_->description().graphicsAPI)
                    };
                    const nri::BarrierDesc barrier{nullptr, 0, &transition, 1, nullptr, 0};
                    core.CmdBarrier(command, barrier);
                    core.CmdZeroBuffer(command, *slot.buffer, 0, slot.buffer_size);
                    transition.before = transition.after;
                    transition.after = {canonical_access, nri::StageBits::ALL};
                    core.CmdBarrier(command, barrier);
                }
            }
        }
    }

    RenderResult<std::span<nri::CommandBuffer* const>>
    RenderExecutor::prepareBatch(RenderGraphInstance& instance, std::uint32_t frame_slot, std::uint32_t batch) noexcept
    {
        auto& frame = instance.frames_[frame_slot];
        auto& device = *instance.session_;
        const auto& description = instance.plan_->submissions()[batch];
        const bool has_tail = description.output_begin != kGraphInvalidIndex;
        const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
        const std::uint32_t end_part = has_tail ? 2u : 1u;
        for (auto part = first_part; part < end_part; ++part)
        {
            const bool reusable = part == 0 && description.reusable_recording;
            if (reusable && frame.recording_revisions[batch] == frame.recorded_revisions[batch]) continue;
            if (part == 0) frame.recorded_revisions[batch] = 0;
            auto* command = frame.command_buffers[batch][part];
            auto result = device.resetCommandAllocator(*frame.command_allocators[batch][part]);
            if (result == nri::Result::SUCCESS) result = device.beginCommandBuffer(*command, reusable);
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            BindingState bindings;
            recordBatchPart(instance, *command, frame_slot, batch, part == 1, bindings);
            result = device.nriCore().EndCommandBuffer(*command);
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            if (reusable) frame.recorded_revisions[batch] = frame.recording_revisions[batch];
        }
        return std::span<nri::CommandBuffer* const>{
            frame.command_buffers[batch].data() + first_part, end_part - first_part
        };
    }

    void RenderExecutor::recordBatchPart(
        RenderGraphInstance& instance,
        nri::CommandBuffer& command,
        std::uint32_t slot,
        std::uint32_t batch_index,
        bool output_tail,
        BindingState& binding_state
    ) noexcept
    {
        const auto& core = instance.session_->nriCore();
        const auto& plan = *instance.plan_;
        auto& frame = instance.frames_[slot];
        const auto& batch = plan.submissions_[batch_index];
        const auto& bindings = frame.bindings;
        const auto readBinding = [&](const ResourceAccess& access) -> const RenderGraphInstance::Binding&
        {
            const auto resource = access.version.resource.index;
            if (access.access == EGraphAccess::READ && access.fallback &&
                !frame.history_valid[plan.resources()[resource].history])
            {
                return bindings[access.fallback->resource.index];
            }
            return bindings[resource];
        };
        // Logical vertex-resource indices are instance-local. Pipeline and descriptor state
        // use physical identities and may be retained across the whole merged recording.
        binding_state.vertices = {};
        const bool has_tail = batch.output_begin != kGraphInvalidIndex;
        const bool first_part = !output_tail || batch.output_begin == batch.first_pass;
        const bool last_part = output_tail || !has_tail;
        if (batch_index == 0 && first_part)
        {
            recordHistoryInitialization(instance, command);
            if (!instance.histories_.empty())
            {
                binding_state = {};
            }
        }
        if (frame.descriptor_pool && binding_state.descriptor_pool != frame.descriptor_pool)
        {
            core.CmdSetDescriptorPool(command, *frame.descriptor_pool);
            binding_state.descriptor_pool = frame.descriptor_pool;
            binding_state.descriptors = {};
        }
        binding_state.shared_descriptors = frame.shared_descriptors;
        auto region = [&](const ResourceAccess& access, const std::optional<ImageCopyRegion>& rectangle = {})
        {
            const auto& range = std::get<GraphImageRange>(access.range);
            const auto& image = core.GetTextureDesc(*readBinding(access).image);
            const auto planes = range.aspects == 1   ? nri::PlaneBits::COLOR
                                : range.aspects == 2 ? nri::PlaneBits::DEPTH
                                                     : nri::PlaneBits::STENCIL;
            return nri::TextureRegionDesc{
                static_cast<nri::Dim_t>(rectangle ? rectangle->x : 0),
                static_cast<nri::Dim_t>(rectangle ? rectangle->y : 0),
                0,
                static_cast<nri::Dim_t>(
                    rectangle ? rectangle->width : std::max(1u, std::uint32_t(image.width) >> range.first_mip)
                ),
                static_cast<nri::Dim_t>(
                    rectangle ? rectangle->height : std::max(1u, std::uint32_t(image.height) >> range.first_mip)
                ),
                1,
                static_cast<nri::Dim_t>(range.first_mip),
                static_cast<nri::Dim_t>(range.first_layer),
                planes
            };
        };
        const auto first_pass = output_tail ? batch.output_begin : batch.first_pass;
        const auto end_pass = !output_tail && has_tail ? batch.output_begin : batch.first_pass + batch.pass_count;
        for (std::size_t p = first_pass; p < end_pass; ++p)
        {
            const auto& pass = frame.passes[p];
            const auto& compiled = *pass.compiled_pass;
            const auto& parameters = pass.parameters;
            if (pass.alias_barrier)
            {
                // Establish the alias memory dependency before any new image layout transition.
                core.CmdBarrier(command, {&*pass.alias_barrier, 1});
            }
            if (!pass.images.empty() || !pass.buffers.empty())
            {
                const nri::BarrierDesc barrier{
                    nullptr,
                    0,
                    pass.buffers.data(),
                    static_cast<std::uint32_t>(pass.buffers.size()),
                    pass.images.data(),
                    static_cast<std::uint32_t>(pass.images.size())
                };
                core.CmdBarrier(command, barrier);
            }
            // Both paths reach the compiled entry state. False preserves the validated predecessor contents.
            if (compiled.condition_region != kGraphInvalidIndex && !frame.enabled_regions[compiled.condition_region])
            {
                continue;
            }
            const auto* raster = std::get_if<RasterCommands>(&compiled.work);
            const auto* compute = std::get_if<ComputeCommands>(&compiled.work);
            if (raster)
            {
                if (const auto* list = std::get_if<IndexedListCommands>(&raster->draw))
                {
                    recordRasterDraws(
                        *instance.session_, command, frame.draw_lists[list->data_index].draws, binding_state
                    );
                    continue;
                }
                if (const auto* dynamic = std::get_if<RasterListCommands>(&raster->draw))
                {
                    const auto& draws = frame.raster_draws[dynamic->data_index];
                    if (draws.sections.empty())
                    {
                        // The complete dynamic stage still observes declared Clear/Load/Store with zero draws.
                        nri::RenderingDesc rendering{};
                        rendering.colors = pass.colors.data();
                        rendering.colorNum = static_cast<std::uint32_t>(pass.colors.size());
                        rendering.depth = pass.depth;
                        core.CmdBeginRendering(command, rendering);
                        core.CmdEndRendering(command);
                    }
                    else
                    {
                        recordRasterDraws(*instance.session_, command, draws, binding_state);
                    }
                    continue;
                }
            }
            const auto& placement = plan.descriptor_plan_->passes[compiled.source_pass];
            if (placement.layout)
            {
                const auto point = raster ? nri::BindPoint::GRAPHICS : nri::BindPoint::COMPUTE;
                const auto& program = plan.descriptor_plan_->passes[compiled.source_pass];
                if (binding_state.layout != program.layout || binding_state.point != point)
                {
                    core.CmdSetPipelineLayout(command, point, *program.layout);
                    binding_state.layout = program.layout;
                    binding_state.point = point;
                    binding_state.descriptors = {};
                }
                auto* pipeline = raster ? raster->pipeline : compute ? compute->pipeline : nullptr;
                if (pipeline && binding_state.pipeline != pipeline)
                {
                    core.CmdSetPipeline(command, *pipeline);
                    binding_state.pipeline = pipeline;
                    binding_state.depth_bias.reset();
                }
                bindDescriptors(
                    *instance.session_, command, program,
                    {&pass.private_descriptors, pass.private_descriptors ? 1u : 0u}, binding_state
                );
                if (!parameters.empty() && (!raster || !std::holds_alternative<IndexedListCommands>(raster->draw)))
                {
                    core.CmdSetRootConstants(
                        command,
                        {0, parameters.data(), static_cast<std::uint32_t>(parameters.size()), 0, point}
                    );
                }
            }
            if (raster)
            {
                const auto& group = plan.recording_.groups[compiled.rendering_group];
                if (group.first_pass == p)
                {
                    nri::RenderingDesc rendering{};
                    rendering.colors = pass.colors.data();
                    rendering.colorNum = static_cast<std::uint32_t>(pass.colors.size());
                    rendering.depth = pass.depth;
                    core.CmdBeginRendering(command, rendering);
                }
                const auto* previous = p ? std::get_if<RasterCommands>(&plan.recording_.passes[p - 1].work) : nullptr;
                if (group.first_pass == p || raster->state_input ||
                    (previous && (previous->state_input || previous->viewport != raster->viewport ||
                                  std::holds_alternative<IndexedListCommands>(previous->draw))))
                {
                    const auto& attachment = raster->colors.empty() ? *raster->depth : raster->colors.front();
                    const auto& image = core.GetTextureDesc(*bindings[attachment.access.version.resource.index].image);
                    const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
                    const auto width = std::max(1u, std::uint32_t(image.width) >> mip);
                    const auto height = std::max(1u, std::uint32_t(image.height) >> mip);
                    const auto& area = raster->state_input ? pass.raster_state.viewport
                                       : raster->viewport  ? *raster->viewport
                                                           : frame.viewport;
                    const nri::Viewport viewport{
                        area.x * width,
                        area.y * height,
                        area.width * width,
                        area.height * height,
                        area.min_depth,
                        area.max_depth,
                        true
                    };
                    const nri::Rect scissor{
                        static_cast<std::int16_t>(area.x * width),
                        static_cast<std::int16_t>(area.y * height),
                        static_cast<nri::Dim_t>(area.width * width),
                        static_cast<nri::Dim_t>(area.height * height)
                    };
                    core.CmdSetViewports(command, &viewport, 1);
                    setScissor(core, command, scissor, binding_state);
                }
                if (raster->state_input)
                {
                    setDepthBias(
                        core, command,
                        {pass.raster_state.depth_bias_constant, 0, pass.raster_state.depth_bias_slope}, binding_state
                    );
                }
                else if (binding_state.depth_bias)
                {
                    setDepthBias(core, command, {raster->depth_bias[0], 0, raster->depth_bias[1]}, binding_state);
                }
                if (!std::ranges::equal(binding_state.vertices, raster->vertices))
                {
                    for (const auto& source : raster->vertices)
                    {
                        const auto& binding = bindings[source.resource.index];
                        const nri::VertexBufferDesc buffer{
                            binding.buffer,
                            binding.buffer_offset + source.offset,
                            source.stride
                        };
                        core.CmdSetVertexBuffers(command, source.binding, &buffer, 1);
                    }
                }
                binding_state.vertices = raster->vertices;
                std::visit(
                    [&](const auto& draw)
                    {
                        using Draw = std::decay_t<decltype(draw)>;
                        if constexpr (std::same_as<Draw, RasterListCommands> || std::same_as<Draw, IndexedListCommands>)
                        {
                            // Prepared lists were consumed before entering the fixed-program path.
                        }
                        else if constexpr (std::is_same_v<Draw, DrawVertices>)
                        {
                            core.CmdDraw(command, {draw.count, draw.instances, draw.first, draw.first_instance});
                        }
                        else
                        {
                            const auto& index = bindings[draw.index_resource.resource.index];
                            const auto offset = [&]()
                            {
                                if constexpr (std::is_same_v<Draw, DrawIndexed>)
                                {
                                    return draw.offset;
                                }
                                else
                                {
                                    return draw.index_offset;
                                }
                            }();
                            const auto index_offset = index.buffer_offset + offset;
                            const auto index_type =
                                draw.type == EIndexType::UINT16 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
                            const bool index_changed = binding_state.index != index.buffer ||
                                                       binding_state.index_offset != index_offset ||
                                                       binding_state.index_type != index_type;
                            if (index_changed)
                            {
                                core.CmdSetIndexBuffer(command, *index.buffer, index_offset, index_type);
                                binding_state.index = index.buffer;
                                binding_state.index_offset = index_offset;
                                binding_state.index_type = index_type;
                            }
                            if constexpr (std::is_same_v<Draw, DrawIndexed>)
                            {
                                core.CmdDrawIndexed(
                                    command,
                                    {draw.count, draw.instances, draw.first, draw.vertex_offset, draw.first_instance}
                                );
                            }
                            else
                            {
                                const auto& indirect = bindings[draw.commands_resource.resource.index];
                                nri::Buffer* count = nullptr;
                                std::uint64_t count_offset = 0;
                                if (draw.count_resource)
                                {
                                    const auto& count_binding = bindings[draw.count_resource->resource.index];
                                    count = count_binding.buffer;
                                    count_offset = count_binding.buffer_offset + draw.count_offset;
                                }
                                core.CmdDrawIndexedIndirect(
                                    command,
                                    *indirect.buffer,
                                    indirect.buffer_offset + draw.commands_offset,
                                    draw.count,
                                    draw.stride,
                                    count,
                                    count_offset
                                );
                            }
                        }
                    },
                    raster->draw
                );
                if (group.first_pass + group.pass_count == p + 1)
                {
                    core.CmdEndRendering(command);
                }
            }
            else if (compute)
            {
                binding_state.vertices = {};
                binding_state.index = nullptr;
                std::visit(
                    [&](const auto& dispatch)
                    {
                        using Dispatch = std::decay_t<decltype(dispatch)>;
                        if constexpr (std::is_same_v<Dispatch, DispatchArguments>)
                        {
                            core.CmdDispatch(command, {dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]});
                        }
                        else if constexpr (std::is_same_v<Dispatch, ImageDispatchArguments>)
                        {
                            const auto& groups = pass.dispatch_groups;
                            core.CmdDispatch(command, {groups[0], groups[1], groups[2]});
                        }
                        else
                        {
                            const auto& buffer = bindings[dispatch.buffer.resource.index];
                            core.CmdDispatchIndirect(command, *buffer.buffer, buffer.buffer_offset + dispatch.offset);
                        }
                    },
                    compute->dispatch
                );
            }
            else
            {
                // Clear helpers may use backend-private compute state. Do not carry a cached
                // application binding across those operations.
                binding_state = {};
                const auto& transfer = std::get<TransferWork>(compiled.work);
                std::uint32_t fill_binding = 0;
                for (std::size_t i = 0; i < transfer.operations.size(); ++i)
                {
                    std::visit(
                        [&](const auto& operation)
                        {
                            using Operation = std::decay_t<decltype(operation)>;
                            const auto& destination = bindings[operation.destination.version.resource.index];
                            if constexpr (std::is_same_v<Operation, ClearImage>)
                            {
                                nri::AttachmentDesc attachment{};
                                attachment.descriptor = pass.clear_views[i];
                                attachment.loadOp = nri::LoadOp::CLEAR;
                                attachment.storeOp = nri::StoreOp::STORE;
                                nri::RenderingDesc rendering{};
                                const bool depth = std::get<GraphImageRange>(operation.destination.range).aspects != 1;
                                if (depth)
                                {
                                    attachment.clearValue.depthStencil.depth = operation.depth;
                                    rendering.depth = attachment;
                                }
                                else
                                {
                                    attachment.clearValue.color.f =
                                        {operation.color[0], operation.color[1], operation.color[2], operation.color[3]
                                        };
                                    rendering.colors = &attachment;
                                    rendering.colorNum = 1;
                                }
                                core.CmdBeginRendering(command, rendering);
                                core.CmdEndRendering(command);
                            }
                            else if constexpr (std::is_same_v<Operation, FillBuffer>)
                            {
                                const auto& range = std::get<GraphBufferRange>(operation.destination.range);
                                if (!operation.value)
                                {
                                    core.CmdZeroBuffer(
                                        command,
                                        *destination.buffer,
                                        destination.buffer_offset + range.offset,
                                        range.size
                                    );
                                }
                                else
                                {
                                    nri::ClearStorageDesc clear{};
                                    clear.descriptor = pass.private_views[fill_binding]
                                                           ? pass.private_views[fill_binding]
                                                           : pass.history_views[fill_binding];
                                    clear.value
                                        .ui = {operation.value, operation.value, operation.value, operation.value};
                                    clear.rangeIndex = fill_binding++;
                                    core.CmdClearStorage(command, clear);
                                }
                            }
                            else
                            {
                                const auto& source = readBinding(operation.source);
                                if constexpr (std::is_same_v<Operation, CopyBuffer>)
                                {
                                    const auto& from = std::get<GraphBufferRange>(operation.source.range);
                                    const auto& to = std::get<GraphBufferRange>(operation.destination.range);
                                    core.CmdCopyBuffer(
                                        command,
                                        *destination.buffer,
                                        destination.buffer_offset + to.offset,
                                        *source.buffer,
                                        source.buffer_offset + from.offset,
                                        from.size
                                    );
                                }
                                else if constexpr (std::is_same_v<Operation, CopyImage>)
                                {
                                    const auto& range = std::get<GraphImageRange>(operation.source.range);
                                    for (std::uint32_t mip = 0; mip < range.mip_count; ++mip)
                                    {
                                        auto from = region(operation.source, operation.source_region);
                                        auto to = region(operation.destination, operation.destination_region);
                                        from.mipOffset += static_cast<nri::Dim_t>(mip);
                                        to.mipOffset += static_cast<nri::Dim_t>(mip);
                                        from.width = to.width =
                                            static_cast<nri::Dim_t>(std::max(1u, std::uint32_t(from.width) >> mip));
                                        from.height = to.height =
                                            static_cast<nri::Dim_t>(std::max(1u, std::uint32_t(from.height) >> mip));
                                        for (std::uint32_t layer = 0; layer < range.layer_count; ++layer)
                                        {
                                            core.CmdCopyTexture(command, *destination.image, &to, *source.image, &from);
                                            ++from.layerOffset;
                                            ++to.layerOffset;
                                        }
                                    }
                                }
                                else
                                {
                                    if (std::holds_alternative<GraphImageRange>(operation.source.range))
                                    {
                                        const auto from = region(operation.source, operation.image_region);
                                        auto layout = pass.copy_layouts[i];
                                        layout.offset += destination.buffer_offset;
                                        core.CmdReadbackTextureToBuffer(
                                            command,
                                            *destination.buffer,
                                            layout,
                                            *source.image,
                                            from
                                        );
                                    }
                                    else
                                    {
                                        const auto to = region(operation.destination, operation.image_region);
                                        auto layout = pass.copy_layouts[i];
                                        layout.offset += source.buffer_offset;
                                        core.CmdUploadBufferToTexture(
                                            command,
                                            *destination.image,
                                            to,
                                            *source.buffer,
                                            layout
                                        );
                                    }
                                }
                            }
                        },
                        transfer.operations[i]
                    );
                }
            }
        }
        if (last_part && batch_index + 1 == plan.submissions_.size() &&
            (!frame.final_images.empty() || !frame.final_buffers.empty()))
        {
            const nri::BarrierDesc barrier{
                nullptr,
                0,
                frame.final_buffers.data(),
                static_cast<std::uint32_t>(frame.final_buffers.size()),
                frame.final_images.data(),
                static_cast<std::uint32_t>(frame.final_images.size())
            };
            core.CmdBarrier(command, barrier);
        }
    }

} // namespace lux::render::detail
