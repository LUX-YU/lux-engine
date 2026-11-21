#include <lux/engine/render/device/ImageFormat.hpp>
#include <algorithm>
#include <cmath>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/graph/ShaderInterface.hpp>
#include <lux/engine/render/graph/compile/BuiltinPass.hpp>

namespace lux::render::detail
{
    namespace
    {
        bool isSameWrite(const ResourceAccess& left, const ResourceAccess& right) noexcept
        {
            return left.version == right.version && left.range == right.range && left.access == right.access;
        }

        RenderResult<void> validateTransfer(const TransferWork& work, std::span<const GraphResource> resources) noexcept
        {
            for (const auto& operation : work.operations)
            {
                const bool valid = std::visit(
                    [&](const auto& copy)
                    {
                        using Operation = std::remove_cvref_t<decltype(copy)>;
                        const auto& destination = resources[copy.destination.version.resource.index].description;
                        const auto* to_buffer = std::get_if<GraphBufferRange>(&copy.destination.range);
                        const auto* to_image = std::get_if<GraphImageRange>(&copy.destination.range);
                        if constexpr (std::is_same_v<Operation, FillBuffer>)
                        {
                            return to_buffer && to_buffer->offset % 4 == 0 && to_buffer->size % 4 == 0;
                        }
                        else if constexpr (std::is_same_v<Operation, ClearImage>)
                        {
                            const bool is_valid_range = to_image && to_image->mip_count == 1;
                            const bool is_valid_clear =
                                std::isfinite(copy.depth) &&
                                std::ranges::all_of(copy.color, [](float value) { return std::isfinite(value); });
                            return is_valid_range && is_valid_clear;
                        }
                        else
                        {
                            const auto& source = resources[copy.source.version.resource.index].description;
                            const auto* from_buffer = std::get_if<GraphBufferRange>(&copy.source.range);
                            const auto* from_image = std::get_if<GraphImageRange>(&copy.source.range);
                            if constexpr (std::is_same_v<Operation, CopyBuffer>)
                            {
                                return from_buffer && to_buffer && from_buffer->size == to_buffer->size;
                            }
                            else if constexpr (std::is_same_v<Operation, CopyImage>)
                            {
                                if (!from_image || !to_image)
                                {
                                    return false;
                                }
                                const auto& a = std::get<GraphImageDesc>(source);
                                const auto& b = std::get<GraphImageDesc>(destination);
                                const auto aw = copy.source_region ? copy.source_region->width
                                                                   : std::max(1u, a.width >> from_image->first_mip);
                                const auto ah = copy.source_region ? copy.source_region->height
                                                                   : std::max(1u, a.height >> from_image->first_mip);
                                const auto bw = copy.destination_region ? copy.destination_region->width
                                                                        : std::max(1u, b.width >> to_image->first_mip);
                                const auto bh = copy.destination_region ? copy.destination_region->height
                                                                        : std::max(1u, b.height >> to_image->first_mip);
                                const bool is_same_format = a.format == b.format && a.samples == b.samples;
                                const bool comparable_mips = a.extent.source == EImageExtentSource::FIXED ||
                                                             from_image->first_mip == to_image->first_mip;
                                const bool is_same_extent =
                                    comparable_mips && aw == bw && ah == bh && a.extent == b.extent;
                                const bool is_same_subresources = from_image->aspects == to_image->aspects &&
                                                                  from_image->mip_count == to_image->mip_count &&
                                                                  from_image->layer_count == to_image->layer_count;
                                return is_same_format && is_same_extent && is_same_subresources;
                            }
                            else
                            {
                                const bool kinds = (from_image && to_buffer) || (from_buffer && to_image);
                                if (!kinds)
                                {
                                    return false;
                                }
                                const auto& image = std::get<GraphImageDesc>(from_image ? source : destination);
                                const auto& range = from_image ? *from_image : *to_image;
                                return image.samples == 1 && range.mip_count == 1 && range.layer_count == 1;
                            }
                        }
                    },
                    operation
                );
                if (!valid)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            return {};
        }

        RenderResult<void> validateDraw(
            const RasterWork::FixedProgram& work, std::span<const GraphResource> resources,
            const nri::DeviceDesc& limits
        ) noexcept
        {
            const auto bufferSize = [&](ResourceVersion version) noexcept
            {
                const auto* desc = std::get_if<GraphBufferDesc>(&resources[version.resource.index].description);
                return desc ? desc->size : 0;
            };
            if (work.vertex_buffers.size() != work.graphics.vertex_bindings.size())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            for (const auto& vertex : work.vertex_buffers)
            {
                const bool duplicate =
                    std::ranges::count(work.vertex_buffers, vertex.binding, &ProgramVertexBuffer::binding) != 1;
                if (duplicate)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const auto binding =
                    std::ranges::find(work.graphics.vertex_bindings, vertex.binding, &VertexBinding::binding);
                if (binding == work.graphics.vertex_bindings.end() || !binding->stride ||
                    vertex.offset >= bufferSize(vertex.resource))
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            return std::visit(
                [&](const auto& draw) noexcept -> RenderResult<void>
                {
                    using Draw = std::remove_cvref_t<decltype(draw)>;
                    if constexpr (!std::is_same_v<Draw, DrawVertices>)
                    {
                        const auto bytes = draw.type == EIndexType::UINT16 ? 2u : 4u;
                        const auto offset = [&]() noexcept
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
                        const auto size = bufferSize(draw.index_resource);
                        const bool invalid_index =
                            draw.type > EIndexType::UINT32 || offset % bytes || offset >= size || size - offset < bytes;
                        if (invalid_index)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        if constexpr (std::is_same_v<Draw, DrawIndexed>)
                        {
                            if (std::uint64_t(draw.first) + draw.count > (size - offset) / bytes)
                            {
                                return cxx::unexpected(RenderError{kDeviceArgument});
                            }
                        }
                    }
                    if constexpr (std::is_same_v<Draw, DrawIndexedIndirect>)
                    {
                        const auto size = bufferSize(draw.commands_resource);
                        const bool invalid_range = !draw.count || draw.commands_offset > size ||
                                                   draw.commands_offset % 4 ||
                                                   draw.stride < sizeof(nri::DrawIndexedDesc) || draw.stride % 4 ||
                                                   (draw.commands_offset <= size &&
                                                    (size - draw.commands_offset < sizeof(nri::DrawIndexedDesc) ||
                                                     std::uint64_t(draw.count - 1) * draw.stride >
                                                         size - draw.commands_offset - sizeof(nri::DrawIndexedDesc)));
                        if (invalid_range || draw.count > limits.other.drawIndirectMaxNum)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        if (draw.count_resource)
                        {
                            const auto count_bytes = bufferSize(*draw.count_resource);
                            if (!limits.features.drawIndirectCount || draw.count_offset % 4 ||
                                draw.count_offset > count_bytes || count_bytes - draw.count_offset < 4)
                            {
                                return cxx::unexpected(RenderError{kDeviceUnsupported});
                            }
                        }
                    }
                    else
                    {
                        if (!draw.count || !draw.instances)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        for (const auto& vertex : work.vertex_buffers)
                        {
                            const auto binding = std::ranges::find(
                                work.graphics.vertex_bindings, vertex.binding, &VertexBinding::binding
                            );
                            std::uint64_t end = 0;
                            if (binding->rate == EVertexRate::INSTANCE)
                            {
                                end = std::uint64_t(draw.first_instance) + draw.instances;
                            }
                            else if constexpr (std::is_same_v<Draw, DrawVertices>)
                            {
                                end = std::uint64_t(draw.first) + draw.count;
                            }
                            if (end > (bufferSize(vertex.resource) - vertex.offset) / binding->stride)
                            {
                                return cxx::unexpected(RenderError{kDeviceArgument});
                            }
                        }
                    }
                    return {};
                },
                work.draw
            );
        }
    } // namespace

    RenderResult<RasterCommands> RasterPass::compile(const RasterWork& value, PassCompileContext& context) noexcept
    {
        const auto* raster = &value;
        const auto* fixed = std::get_if<RasterWork::FixedProgram>(&value.program);
        const auto& limits = context.limits;
        RasterCommands recorded;
        if (fixed)
        {
            if (auto valid = validateDraw(*fixed, context.resources, limits); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            recorded.draw = std::visit([](const auto& draw) noexcept -> VRasterDraw
            {
                using Draw = std::remove_cvref_t<decltype(draw)>;
                if constexpr (std::same_as<Draw, DrawVertices>)
                    return nri::DrawDesc{draw.count, draw.instances, draw.first, draw.first_instance};
                else
                {
                    const auto type = draw.type == EIndexType::UINT16 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
                    const auto buffer = draw.index_resource.resource.index;
                    if constexpr (std::same_as<Draw, DrawIndexed>)
                    {
                        return IndexedDrawPacket{
                            {{buffer, draw.offset}, type},
                            {draw.count, draw.instances, draw.first, draw.vertex_offset, draw.first_instance}
                        };
                    }
                    else
                    {
                        const auto count = draw.count_resource ? draw.count_resource->resource.index : kGraphInvalidIndex;
                        return IndirectDrawPacket{
                            {{buffer, draw.index_offset}, type}, {draw.commands_resource.resource.index, draw.commands_offset},
                            {count, draw.count_offset}, draw.count, draw.stride
                        };
                    }
                }
            }, fixed->draw);
            for (const auto& source : fixed->vertex_buffers)
            {
                const auto binding =
                    std::ranges::find(fixed->graphics.vertex_bindings, source.binding, &VertexBinding::binding);
                recorded.vertices.push_back({
                    source.resource.resource.index, source.binding, source.offset, binding->stride
                });
            }
        }
        else
        {
            const auto& stage = std::get<RasterPassTemplate>(raster->program);
            recorded.draw = RasterListCommands{(*context.raster_list_count)++, stage.usesStreamProducer()};
        }
        return recorded;
    }

    RenderResult<ComputeCommands> ComputePass::compile(const ComputeWork& value, PassCompileContext& context) noexcept
    {
        if (const auto* direct = std::get_if<DispatchArguments>(&value.dispatch))
        {
            const auto& limit = context.limits.shaderStage.compute.dispatchMaxDim;
            for (unsigned axis = 0; axis < direct->groups.size(); ++axis)
            {
                if (!direct->groups[axis] || direct->groups[axis] > limit[axis])
                    return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return ComputeCommands{nri::DispatchDesc{direct->groups[0], direct->groups[1], direct->groups[2]}};
        }
        if (const auto* dispatch = std::get_if<ImageDispatchArguments>(&value.dispatch))
        {
            const auto slot = dispatch->resource.index;
            const bool materialized = slot < context.materialized_resources.size() &&
                context.materialized_resources[slot];
            const auto* image = slot < context.resources.size()
                ? std::get_if<GraphImageDesc>(&context.resources[slot].description) : nullptr;
            const bool valid_image = materialized && image && dispatch->mip < image->mips;
            const bool valid_group = dispatch->group_size[0] && dispatch->group_size[1];
            if (!valid_image || !valid_group) return cxx::unexpected(RenderError{kDeviceArgument});
            return ComputeCommands{ImageDispatchExtent{slot, dispatch->mip, dispatch->group_size}};
        }
        const auto& indirect = std::get<IndirectDispatchArguments>(value.dispatch);
        const auto slot = indirect.buffer.resource.index;
        const auto& buffer = std::get<GraphBufferDesc>(context.resources[slot].description);
        const bool fits = indirect.offset <= buffer.size && buffer.size - indirect.offset >= sizeof(nri::DispatchDesc);
        if (indirect.offset % 4 || !fits) return cxx::unexpected(RenderError{kDeviceArgument});
        return ComputeCommands{BufferLocation{slot, indirect.offset}};
    }

    RenderResult<TransferCommands>
    TransferPass::compile(
        const TransferWork& value, std::span<const ResourceAccess> storage_fills, PassCompileContext& context
    ) noexcept
    {
        if (auto valid = validateTransfer(value, context.resources); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto fallback = [](const ResourceAccess& access) noexcept
        {
            const bool reads_fallback = access.access == EGraphAccess::READ && access.fallback;
            return reads_fallback ? access.fallback->resource.index : kGraphInvalidIndex;
        };
        const auto buffer = [&](const ResourceAccess& access) noexcept
        {
            return BufferOperand{
                access.version.resource.index, fallback(access), std::get<GraphBufferRange>(access.range)
            };
        };
        const auto image = [&](const ResourceAccess& access, std::optional<ImageCopyRegion> region = {}) noexcept
        {
            const auto slot = access.version.resource.index;
            const auto format = std::get<GraphImageDesc>(context.resources[slot].description).format;
            const auto& range = std::get<GraphImageRange>(access.range);
            return ImageOperand{
                slot, fallback(access), range, region, imageFormat(format).block,
                rdesc::textureBlockBytes(format, range.aspects)
            };
        };
        TransferCommands commands;
        commands.operations.reserve(value.operations.size());
        const bool serialize_buffer_writes = context.limits.graphicsAPI == nri::GraphicsAPI::D3D12;
        std::vector<std::uint32_t> filled_resources;
        std::uint32_t next_descriptor = 0;
        for (const auto& operation : value.operations)
        {
            if (const auto* copy = std::get_if<CopyBuffer>(&operation))
                commands.operations.emplace_back(BufferCopyPacket{buffer(copy->source), buffer(copy->destination)});
            else if (const auto* copy = std::get_if<CopyImage>(&operation))
                commands.operations.emplace_back(ImageCopyPacket{
                    image(copy->source, copy->source_region), image(copy->destination, copy->destination_region)
                });
            else if (const auto* copy = std::get_if<CopyImageBuffer>(&operation))
            {
                const bool readback = std::holds_alternative<GraphImageRange>(copy->source.range);
                commands.operations.emplace_back(ImageBufferPacket{
                    image(readback ? copy->source : copy->destination, copy->image_region),
                    buffer(readback ? copy->destination : copy->source),
                    readback ? EImageBufferCopy::READBACK : EImageBufferCopy::UPLOAD, copy->row_pitch, copy->slice_pitch
                });
            }
            else if (const auto* clear = std::get_if<ClearImage>(&operation))
                commands.operations.emplace_back(ClearPacket{
                    image(clear->destination), clear->color, clear->depth, commands.clear_view_count++
                });
            else
            {
                const auto& fill = std::get<FillBuffer>(operation);
                FillPacket packet{buffer(fill.destination), fill.value, kGraphInvalidIndex, false};
                const bool uses_storage = next_descriptor < storage_fills.size() &&
                    isSameWrite(fill.destination, storage_fills[next_descriptor]);
                if (uses_storage)
                    packet.descriptor = next_descriptor++;
                if (packet.value && packet.descriptor == kGraphInvalidIndex)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                if (serialize_buffer_writes)
                {
                    const auto slot = packet.destination.slot;
                    packet.sync_before = std::ranges::find(filled_resources, slot) != filled_resources.end();
                    if (!packet.sync_before) filled_resources.push_back(slot);
                }
                commands.operations.emplace_back(packet);
            }
        }
        if (next_descriptor != storage_fills.size())
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        return commands;
    }

    std::optional<PassNativeAccess> TransferPass::access(
        const TransferWork& work, std::span<const ResourceAccess> storage_fills, const ResourceAccess& use,
        const VGraphResourceDesc& description, nri::GraphicsAPI backend
    ) noexcept
    {
        for (const auto& operation : work.operations)
        {
            if (const auto* clear = std::get_if<ClearImage>(&operation);
                clear && isSameWrite(clear->destination, use))
            {
                const auto& image = std::get<GraphImageDesc>(description);
                const bool depth = rdesc::textureAspectMask(image.format) != 1;
                return PassNativeAccess{
                    depth ? nri::AccessBits::DEPTH_STENCIL_ATTACHMENT : nri::AccessBits::COLOR_ATTACHMENT,
                    depth ? nri::StageBits::DEPTH_STENCIL_ATTACHMENT : nri::StageBits::COLOR_ATTACHMENT,
                    depth ? nri::Layout::DEPTH_STENCIL_ATTACHMENT : nri::Layout::COLOR_ATTACHMENT
                };
            }
            if (const auto* fill = std::get_if<FillBuffer>(&operation);
                fill && isSameWrite(fill->destination, use))
            {
                const bool storage = std::ranges::any_of(storage_fills, [&](const auto& write) noexcept
                {
                    return isSameWrite(write, use);
                });
                return PassNativeAccess{
                    storage ? nri::AccessBits::CLEAR_STORAGE : nri::AccessBits::COPY_DESTINATION,
                    storage ? nri::StageBits::CLEAR_STORAGE : zeroBufferStages(backend)
                };
            }
        }
        return std::nullopt;
    }
} // namespace lux::render::detail
