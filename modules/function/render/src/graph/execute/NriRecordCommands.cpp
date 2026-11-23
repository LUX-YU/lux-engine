#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/execute/PassBindings.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/device/CopyEncoding.hpp>
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

    } // namespace

    void RasterEncoder::record(const RasterDrawList& list) const noexcept
    {
        nri::Pipeline* pipeline = nullptr;
        nri::PipelineLayout* layout = nullptr;
        std::array<nri::DescriptorSet*, 4> bound_sets{};
        const auto bindSet = [&](std::uint32_t set, nri::DescriptorSet* value)
        {
            if (bound_sets[set] == value) return;
            core.CmdSetDescriptorSet(command, {set, value, nri::BindPoint::GRAPHICS});
            bound_sets[set] = value;
        };
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
                if (layout != packet.bindings.layout)
                {
                    core.CmdSetPipelineLayout(command, nri::BindPoint::GRAPHICS, *packet.bindings.layout);
                    layout = packet.bindings.layout;
                    bound_sets = {};
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
                if (pipeline != packet.pipeline)
                {
                    core.CmdSetPipeline(command, *packet.pipeline);
                    pipeline = packet.pipeline;
                    state.depth_bias.reset();
                }
                if (section.depth_bias)
                {
                    setPassDepthBias(core, command, *section.depth_bias, state);
                }
                const auto descriptors =
                    std::span(list.descriptors).subspan(packet.first_descriptor, packet.descriptor_count);
                for (std::size_t kind = 0; kind < frame.shared_descriptors.size(); ++kind)
                {
                    const auto set = packet.bindings.shared_sets[kind];
                    if (set != kGraphInvalidIndex) bindSet(set, frame.shared_descriptors[kind]);
                }
                for (std::uint32_t i = 0; i < descriptors.size(); ++i)
                    bindSet(packet.bindings.private_set + i, descriptors[i]);
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

    void RasterEncoder::record(const RasterCommands& raster, const FramePass& pass) const noexcept
    {
        const auto& bindings = frame.resources;
        if (const auto* dynamic = std::get_if<RasterListCommands>(&raster.draw))
        {
            const auto& draws = frame.draws[dynamic->data_index];
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
                record(draws);
            }
            return;
        }

        if (!std::ranges::equal(state.vertices, raster.vertices))
        {
            for (const auto& source : raster.vertices)
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
        state.vertices = raster.vertices;
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
                    const bool index_changed = state.index != index.buffer ||
                                               state.index_offset != index_offset ||
                                               state.index_type != index_type;
                    if (index_changed)
                    {
                        core.CmdSetIndexBuffer(command, *index.buffer, index_offset, index_type);
                        state.index = index.buffer;
                        state.index_offset = index_offset;
                        state.index_type = index_type;
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
            raster.draw
        );
    }

    void recordTransfers(
        const TransferCommands& range, const FrozenRecordingFrame& frame,
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
