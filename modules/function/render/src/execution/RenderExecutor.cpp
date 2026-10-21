#include <algorithm>
#include <cstring>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/BindingState.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/RenderExecutor.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>

namespace lux::render::detail
{
    void RenderExecutor::record(RenderGraphInstance& instance, nri::CommandBuffer& command, std::uint32_t slot) noexcept
    {
        const auto& core = instance.session_->nriCore();
        const auto& plan = *instance.plan_;
        const auto& frame = instance.frames_[slot];
        const auto& bindings = frame.bindings;
        BindingState binding_state;
        auto region = [&](const ResourceAccess& access, const std::optional<ImageCopyRegion>& rectangle = {})
        {
            const auto& range = std::get<GraphImageRange>(access.range);
            const auto& image = core.GetTextureDesc(*bindings[access.version.resource.index].image);
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
        for (std::size_t p = 0; p < frame.passes.size(); ++p)
        {
            const auto& pass = frame.passes[p];
            const auto& compiled = *pass.compiled_pass;
            const auto& parameters = instance.pass_parameters_[p];
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
            const auto* raster = std::get_if<RasterCommands>(&compiled.work);
            const auto* compute = std::get_if<ComputeCommands>(&compiled.work);
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
                }
                if (compiled.pipeline && binding_state.pipeline != compiled.pipeline)
                {
                    core.CmdSetPipeline(command, *compiled.pipeline);
                    binding_state.pipeline = compiled.pipeline;
                }
                for (std::uint32_t set = 0; set < pass.descriptors.size(); ++set)
                {
                    core.CmdSetDescriptorSet(command, {set, pass.descriptors[set], point});
                }
                if (!parameters.empty())
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
                    const auto& attachment = raster->colors.empty() ? *raster->depth : raster->colors.front();
                    const auto& image = core.GetTextureDesc(*bindings[attachment.access.version.resource.index].image);
                    const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
                    const auto width = std::max(1u, std::uint32_t(image.width) >> mip);
                    const auto height = std::max(1u, std::uint32_t(image.height) >> mip);
                    const auto& area = instance.viewport_;
                    const nri::Viewport viewport{
                        area.x * width,
                        area.y * height,
                        area.width * width,
                        area.height * height,
                        area.min_depth,
                        area.max_depth,
                        true
                    };
                    const nri::Rect scissor{0, 0, static_cast<nri::Dim_t>(width), static_cast<nri::Dim_t>(height)};
                    core.CmdSetViewports(command, &viewport, 1);
                    core.CmdSetScissors(command, &scissor, 1);
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
                        if constexpr (std::is_same_v<Draw, DrawVertices>)
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
                binding_state.vertices = {};
                binding_state.index = nullptr;
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
                                    clear.descriptor = pass.private_views[fill_binding];
                                    clear.value
                                        .ui = {operation.value, operation.value, operation.value, operation.value};
                                    clear.rangeIndex = fill_binding++;
                                    core.CmdClearStorage(command, clear);
                                }
                            }
                            else
                            {
                                const auto& source = bindings[operation.source.version.resource.index];
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
        if (!frame.final_images.empty() || !frame.final_buffers.empty())
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
