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

    void
    RasterEncoder::beginRendering(const nri::RenderingDesc& rendering, bool initialize_resolve_targets) const noexcept
    {
        if (initialize_resolve_targets)
        {
            // Legacy D3D12 requires a clear, discard or copy before the first resolve into a placed RT.
            // NRI resolves the entire selected destination subresource. Use the actual section views.
            for (std::uint32_t i = 0; i < rendering.colorNum; ++i)
            {
                if (!rendering.colors[i].resolveDst) continue;
                nri::AttachmentDesc clear{};
                clear.descriptor = rendering.colors[i].resolveDst;
                clear.loadOp = nri::LoadOp::CLEAR;
                clear.storeOp = nri::StoreOp::STORE;
                nri::RenderingDesc initialize{};
                initialize.colors = &clear;
                initialize.colorNum = 1;
                core.CmdBeginRendering(command, initialize);
                core.CmdEndRendering(command);
            }
        }
        core.CmdBeginRendering(command, rendering);
    }

    void RasterEncoder::bindVertices(std::span<const FrozenVertexBinding> vertices) const noexcept
    {
        if (std::ranges::equal(state.vertices, vertices)) return;
        for (const auto& vertex : vertices) core.CmdSetVertexBuffers(command, vertex.binding, &vertex.buffer, 1);
        state.vertices = vertices;
    }

    void RasterEncoder::record(const RasterDrawList& list, bool initialize_resolve_targets) const noexcept
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
        for (const auto& section : list.recordSections())
        {
            const auto colors = std::span(list.colors).subspan(section.first_color, section.color_count);
            nri::RenderingDesc rendering{};
            rendering.colors = colors.data();
            rendering.colorNum = static_cast<std::uint32_t>(colors.size());
            rendering.depth = section.depth;
            beginRendering(rendering, initialize_resolve_targets);
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
                for (std::size_t kind = 0; kind < frame.shared_descriptors.size(); ++kind)
                {
                    const auto set = packet.bindings.shared_sets[kind];
                    if (set != kGraphInvalidIndex) bindSet(set, frame.shared_descriptors[kind]);
                }
                if (packet.descriptor_set) bindSet(packet.bindings.private_set, packet.descriptor_set);
                bindVertices(std::span(list.vertices).subspan(packet.first_vertex, packet.vertex_count));
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
        if (const auto* dynamic = std::get_if<RasterListCommands>(&raster.draw))
        {
            const auto& draws = frame.draws[dynamic->data_index];
            const auto& scope = *frame.plan.recordingProgram().rasterScope(*pass.compiled_pass);
            if (draws.recordSections().empty())
            {
                // The complete dynamic stage still observes declared Clear/Load/Store with zero draws.
                nri::RenderingDesc rendering{};
                rendering.colors = pass.colors.data();
                rendering.colorNum = static_cast<std::uint32_t>(pass.colors.size());
                rendering.depth = pass.depth;
                beginRendering(rendering, scope.initialize_resolve_targets);
                core.CmdEndRendering(command);
            }
            else
            {
                record(draws, scope.initialize_resolve_targets);
            }
            return;
        }

        bindVertices(frame.vertices.subspan(raster.first_vertex, raster.vertex_count));
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
                    const auto& index = frame.raster_addresses[draw.index.address];
                    const auto index_offset = index.offset;
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
                        const auto& indirect = frame.raster_addresses[draw.arguments];
                        FrozenBufferAddress count{};
                        if (draw.count_buffer != kGraphInvalidIndex) count = frame.raster_addresses[draw.count_buffer];
                        core.CmdDrawIndexedIndirect(
                            command,
                            *indirect.buffer,
                            indirect.offset,
                            draw.count,
                            draw.stride,
                            count.buffer,
                            count.offset
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
            case ETransferOpcode::READBACK:
            {
                const auto& copy = operands.image_buffers[row];
                auto region = copy.region;
                auto layout = copy.layout;
                for (std::uint32_t z = 0; z < copy.slice_copies; ++z)
                {
                    if (opcode == ETransferOpcode::UPLOAD) recordUpload(core, command, {*copy.image, region, *copy.buffer, layout});
                    else recordReadback(core, command, {*copy.buffer, layout, *copy.image, region});
                    ++region.z;
                    layout.offset += layout.slicePitch;
                }
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
