#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/graph/execute/PassBindings.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <algorithm>

namespace lux::render::detail
{
    namespace
    {
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
                             packet.parameter_destination,
                             nri::BindPoint::GRAPHICS}
                        );
                    }
                    setPassScissor(core, command, packet.scissor, state);
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
                                    core.CmdSetIndexBuffer(
                                        command, *packet.index, packet.index_offset, packet.index_type
                                    );
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

        if (const auto* list = std::get_if<IndexedListCommands>(&raster->draw))
        {
            recordRasterDraws(
                core, pass.shared_descriptors, command, *pass.draws, binding_state
            );
            return {};
        }
        if (const auto* dynamic = std::get_if<RasterListCommands>(&raster->draw))
        {
            const auto& draws = *pass.draws;
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
                recordRasterDraws(core, pass.shared_descriptors, command, draws, binding_state);
            }
            return {};
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
        return {};
    }

    RenderResult<void>
    ComputeCommands::record(
        const PassRecordInput& input,
        nri::CommandBuffer& command,
        const nri::CoreInterface& core
    ) const noexcept
    {
        if (!input.native || !input.native->bindings) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& pass = *input.native;
        auto& binding_state = *pass.bindings;
        const auto& bindings = input.resources;

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
            dispatch
        );
        return {};
    }

    RenderResult<void>
    TransferCommands::record(
        const PassRecordInput& input,
        nri::CommandBuffer& command,
        const nri::CoreInterface& core
    ) const noexcept
    {
        if (!input.native || !input.native->bindings) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& pass = *input.native;
        auto& binding_state = *pass.bindings;
        const auto& bindings = input.resources;
        const auto readBinding = [&](const ResourceAccess& access) -> const PassResourceInput&
        {
            const auto& resource = bindings[access.version.resource.index];
            if (access.access == EGraphAccess::READ && access.fallback && !resource.history_valid)
                return bindings[access.fallback->resource.index];
            return resource;
        };
        const auto region = [&](const ResourceAccess& access, const std::optional<ImageCopyRegion>& rectangle = {})
        {
            const auto& range = std::get<GraphImageRange>(access.range);
            const auto& image = readBinding(access);
            const auto planes = range.aspects == 1 ? nri::PlaneBits::COLOR
                : range.aspects == 2 ? nri::PlaneBits::DEPTH : nri::PlaneBits::STENCIL;
            return nri::TextureRegionDesc{
                static_cast<nri::Dim_t>(rectangle ? rectangle->x : 0),
                static_cast<nri::Dim_t>(rectangle ? rectangle->y : 0), 0,
                static_cast<nri::Dim_t>(rectangle ? rectangle->width : std::max(1u, image.width >> range.first_mip)),
                static_cast<nri::Dim_t>(rectangle ? rectangle->height : std::max(1u, image.height >> range.first_mip)),
                1, static_cast<nri::Dim_t>(range.first_mip), static_cast<nri::Dim_t>(range.first_layer), planes
            };
        };

        // Clear helpers may use backend-private compute state. Do not carry a cached
        // application binding across those operations.
        binding_state = {};
        std::uint32_t fill_binding = 0;
        for (std::size_t i = 0; i < operations.size(); ++i)
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
                            recordCopy(core, command, BufferCopyRegion{
                                source.buffer, destination.buffer, source.buffer_offset + from.offset,
                                destination.buffer_offset + to.offset, from.size
                            });
                        }
                        else if constexpr (std::is_same_v<Operation, CopyImage>)
                        {
                            const auto& range = std::get<GraphImageRange>(operation.source.range);
                            recordCopy(core, command, TextureCopyRegion{
                                source.image, destination.image,
                                region(operation.source, operation.source_region),
                                region(operation.destination, operation.destination_region),
                                range.mip_count, range.layer_count
                            });
                        }
                        else
                        {
                            if (std::holds_alternative<GraphImageRange>(operation.source.range))
                            {
                                const auto from = region(operation.source, operation.image_region);
                                auto layout = pass.copy_layouts[i];
                                layout.offset += destination.buffer_offset;
                                recordReadback(
                                    core, command,
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
                                recordUpload(
                                    core, command,
                                    *destination.image,
                                    to,
                                    *source.buffer,
                                    layout
                                );
                            }
                        }
                    }
                },
                operations[i]
            );
        }
        return {};
    }

} // namespace lux::render::detail
