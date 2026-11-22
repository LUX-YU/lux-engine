#include <lux/engine/render/graph/execute/FrameRecordInput.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/execute/PassBindings.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <algorithm>

namespace lux::render::detail
{
    namespace
    {
        void orderFill(
            nri::CommandBuffer& command, const nri::CoreInterface& core, const FrozenBufferFill& fill, nri::AccessStage state
        ) noexcept
        {
            const nri::BufferBarrierDesc barrier{fill.buffer, state, state};
            nri::BarrierDesc barriers{};
            barriers.buffers = &barrier;
            barriers.bufferNum = 1;
            core.CmdBarrier(command, barriers);
        }

        void
        recordRasterDraws(
            const nri::CoreInterface& core,
            std::span<nri::DescriptorSet* const, 3> shared_descriptors,
            nri::CommandBuffer& command,
            const RasterDrawList& list,
            BindingState& state
        ) noexcept
        {
            // Graph-local vertex indices cannot describe the physical arrays in a dynamic packet.
            state.vertices = {};
            std::span<const RasterDrawList::Vertex> previous_vertices;
            for (const auto& section : list.recordSections())
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
                    if (state.layout != packet.bindings.layout || state.point != nri::BindPoint::GRAPHICS)
                    {
                        core.CmdSetPipelineLayout(command, nri::BindPoint::GRAPHICS, *packet.bindings.layout);
                        state.layout = packet.bindings.layout;
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
                        setPassDepthBias(core, command, *section.depth_bias, state);
                    }
                    const auto descriptors =
                        std::span(list.descriptors).subspan(packet.first_descriptor, packet.descriptor_count);
                    bindPassDescriptors(core, shared_descriptors, command, packet.bindings, descriptors, state);
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
                             0,
                             nri::BindPoint::GRAPHICS}
                        );
                    }
                    setPassScissor(core, command, packet.scissor, state);
                    const bool changed_index = state.index != packet.index ||
                        state.index_offset != packet.index_offset || state.index_type != packet.index_type;
                    if (packet.index && changed_index)
                    {
                        core.CmdSetIndexBuffer(command, *packet.index, packet.index_offset, packet.index_type);
                        state.index = packet.index;
                        state.index_offset = packet.index_offset;
                        state.index_type = packet.index_type;
                    }
                    std::visit(
                        [&](const auto& draw) noexcept
                        {
                            using Draw = std::remove_cvref_t<decltype(draw)>;
                            if constexpr (std::same_as<Draw, nri::DrawDesc>)
                            {
                                core.CmdDraw(command, draw);
                            }
                            else if constexpr (std::same_as<Draw, nri::DrawIndexedDesc>)
                            {
                                core.CmdDrawIndexed(command, draw);
                            }
                            else
                            {
                                const auto record = packet.index ? core.CmdDrawIndexedIndirect : core.CmdDrawIndirect;
                                record(
                                    command,
                                    *draw.arguments,
                                    draw.offset,
                                    draw.count,
                                    draw.stride,
                                    draw.count_buffer,
                                    draw.count_offset
                                );
                            }
                        },
                        packet.draw
                    );
                }
                core.CmdEndRendering(command);
            }
        }
    } // namespace

    RenderResult<void>
    RasterCommands::record(
        const PassRecordInput& input,
        nri::CommandBuffer& command,
        const nri::CoreInterface& core
    ) const noexcept
    {
        if (!input.native || !input.native->bindings) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& pass = *input.native;
        auto& binding_state = *pass.bindings;
        const auto& bindings = input.resources;
        const auto* raster = this;

        if (const auto* dynamic = std::get_if<RasterListCommands>(&raster->draw))
        {
            const auto& draws = *pass.draws;
            if (draws.recordSections().empty())
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
                recordRasterDraws(core, input.shared_descriptors, command, draws, binding_state);
            }
            return {};
        }

        if (!std::ranges::equal(binding_state.vertices, raster->vertices))
        {
            for (const auto& source : raster->vertices)
            {
                const auto& binding = bindings[source.buffer];
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
                if constexpr (std::same_as<Draw, RasterListCommands>)
                {
                    // Prepared lists were consumed before entering the fixed-program path.
                }
                else if constexpr (std::same_as<Draw, nri::DrawDesc>)
                {
                    core.CmdDraw(command, draw);
                }
                else
                {
                    const auto& index = bindings[draw.index.location.buffer];
                    const auto index_offset = index.buffer_offset + draw.index.location.offset;
                    const auto index_type = draw.index.type;
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
                    if constexpr (std::same_as<Draw, IndexedDrawPacket>)
                    {
                        core.CmdDrawIndexed(command, draw.draw);
                    }
                    else
                    {
                        const auto& indirect = bindings[draw.arguments.buffer];
                        nri::Buffer* count = nullptr;
                        std::uint64_t count_offset = 0;
                        if (draw.count_buffer.buffer != kGraphInvalidIndex)
                        {
                            const auto& count_binding = bindings[draw.count_buffer.buffer];
                            count = count_binding.buffer;
                            count_offset = count_binding.buffer_offset + draw.count_buffer.offset;
                        }
                        core.CmdDrawIndexedIndirect(
                            command,
                            *indirect.buffer,
                            indirect.buffer_offset + draw.arguments.offset,
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
        return {};
    }

    void
    recordCompute(const PassNativeInput& pass, nri::CommandBuffer& command, const nri::CoreInterface& core) noexcept
    {
        pass.bindings->vertices = {};
        pass.bindings->index = nullptr;
        if (pass.indirect_buffer) core.CmdDispatchIndirect(command, *pass.indirect_buffer, pass.indirect_offset);
        else core.CmdDispatch(command, pass.dispatch);
    }

    void recordTransfers(
        const TransferCommands& range, const FrameRecordInput& frame,
        nri::CommandBuffer& command, const nri::CoreInterface& core
    ) noexcept
    {
        const auto& program = frame.plan.recordingProgram().transfers;
        const auto& operands = frame.transfers;
        for (const auto [opcode, row] : program.commands(range))
        {
            switch (opcode)
            {
            case ETransferOpcode::COPY_BUFFER: recordCopy(core, command, operands.buffer_copies[row]); break;
            case ETransferOpcode::COPY_IMAGE: recordCopy(core, command, operands.image_copies[row]); break;
            case ETransferOpcode::UPLOAD:
            {
                const auto& copy = operands.image_buffers[row];
                recordUpload(core, command, {*copy.image, copy.region, *copy.buffer, copy.layout});
                break;
            }
            case ETransferOpcode::READBACK:
            {
                const auto& copy = operands.image_buffers[row];
                recordReadback(core, command, {*copy.buffer, copy.layout, *copy.image, copy.region});
                break;
            }
            case ETransferOpcode::CLEAR_COLOR:
            {
                nri::RenderingDesc rendering{};
                rendering.colors = &operands.clears[row];
                rendering.colorNum = 1;
                core.CmdBeginRendering(command, rendering);
                core.CmdEndRendering(command);
                break;
            }
            case ETransferOpcode::CLEAR_DEPTH:
            {
                nri::RenderingDesc rendering{};
                rendering.depth = operands.clears[row];
                core.CmdBeginRendering(command, rendering);
                core.CmdEndRendering(command);
                break;
            }
            case ETransferOpcode::ZERO_BUFFER:
            {
                const auto& fill = program.fills[row];
                const auto& native = operands.fills[row];
                if (fill.sync_before)
                    orderFill(command, core, native, {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY});
                core.CmdZeroBuffer(command, *native.buffer, native.offset, fill.destination.range.size);
                break;
            }
            case ETransferOpcode::FILL_STORAGE:
            {
                const auto& fill = program.fills[row];
                const auto& native = operands.fills[row];
                if (fill.sync_before)
                    orderFill(command, core, native, {nri::AccessBits::CLEAR_STORAGE, nri::StageBits::CLEAR_STORAGE});
                nri::ClearStorageDesc clear{};
                clear.descriptor = native.storage_view;
                clear.value.ui = {fill.value, fill.value, fill.value, fill.value};
                clear.rangeIndex = fill.descriptor;
                core.CmdClearStorage(command, clear);
                break;
            }
            }
        }
    }
} // namespace lux::render::detail
