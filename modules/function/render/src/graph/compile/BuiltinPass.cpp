#include <algorithm>
#include <cmath>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/ShaderInterface.hpp>
#include <lux/engine/render/graph/compile/BuiltinPass.hpp>

namespace lux::render::detail
{
    namespace
    {
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
                    else if constexpr (std::is_same_v<Draw, DrawIndexedList>)
                    {
                        const bool invalid_list = work.vertex_buffers.size() != 1 || draw.index_offset != 0 ||
                                                  work.vertex_buffers.front().offset != 0 ||
                                                  draw.selection_parameter.empty();
                        if (invalid_list)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
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
            if (const auto* list = std::get_if<DrawIndexedList>(&fixed->draw))
            {
                const auto reflected = shaderInterface(*context.shader);
                if (!reflected)
                {
                    return cxx::unexpected(reflected.error());
                }
                const auto& fields = reflected->parameters.fields;
                const auto field = std::ranges::find(fields, list->selection_parameter, &rdesc::ShaderMember::path);
                const bool missing_selection = field == fields.end();
                const bool wrong_selection =
                    !missing_selection && (field->scalar != rdesc::EDataScalar::UINT || field->rows != 1 ||
                                           field->columns != 1 || field->array_stride != 0);
                if (missing_selection || wrong_selection)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                recorded.draw = IndexedListCommands{
                    list->index_resource, list->index_offset, list->type, field->offset, (*context.indexed_list_count)++
                };
                auto& compiled_list = std::get<IndexedListCommands>(recorded.draw);
                compiled_list.partial_parameters = limits.features.rootConstantsOffset;
            }
            else
            {
                std::visit(
                    [&](const auto& draw) noexcept
                    {
                        if constexpr (!std::same_as<std::decay_t<decltype(draw)>, DrawIndexedList>)
                        {
                            recorded.draw = draw;
                        }
                    },
                    fixed->draw
                );
            }
            for (const auto& source : fixed->vertex_buffers)
            {
                const auto binding =
                    std::ranges::find(fixed->graphics.vertex_bindings, source.binding, &VertexBinding::binding);
                recorded.vertices.push_back({source.resource.resource, source.binding, source.offset, binding->stride});
            }
        }
        else
        {
            recorded.draw =
                RasterListCommands{(*context.raster_list_count)++, std::get<RasterPassTemplate>(raster->program)};
        }
        return recorded;
    }

    RenderResult<ComputeCommands> ComputePass::compile(const ComputeWork& value, PassCompileContext& context) noexcept
    {
        const auto* compute = &value;
        const auto& limits = context.limits;
        if (const auto* direct = std::get_if<DispatchArguments>(&compute->dispatch))
        {
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                if (!direct->groups[axis] || direct->groups[axis] > limits.shaderStage.compute.dispatchMaxDim[axis])
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
        }
        else if (const auto* dispatch = std::get_if<ImageDispatchArguments>(&compute->dispatch))
        {
            const auto resource = dispatch->resource.index;
            const auto* image = resource < context.resources.size()
                                    ? std::get_if<GraphImageDesc>(&context.resources[resource].description)
                                    : nullptr;
            const bool invalid =
                !image || dispatch->mip >= image->mips || !dispatch->group_size[0] || !dispatch->group_size[1] || resource >= context.materialized_resources.size() ||
                    !context.materialized_resources[resource];
            if (invalid)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        else
        {
            const auto& indirect = std::get<IndirectDispatchArguments>(compute->dispatch);
            const auto& description =
                std::get<GraphBufferDesc>(context.resources[indirect.buffer.resource.index].description);
            const bool invalid =
                indirect.offset % 4 || indirect.offset > description.size || description.size - indirect.offset < 12;
            if (invalid)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        return ComputeCommands{value.dispatch};
    }

    RenderResult<TransferCommands>
    TransferPass::compile(const TransferWork& value, PassCompileContext& context) noexcept
    {
        if (auto valid = validateTransfer(value, context.resources); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return TransferCommands{value.operations};
    }
} // namespace lux::render::detail

namespace lux::render::detail
{
    std::optional<PassNativeAccess> TransferPass::access(
        const TransferWork& work, const ResourceAccess& use, const VGraphResourceDesc& description,
        nri::GraphicsAPI backend
    ) noexcept
    {
        for (const auto& operation : work.operations)
        {
            if (const auto* clear = std::get_if<ClearImage>(&operation);
                clear && clear->destination.version.resource == use.version.resource)
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
                fill && fill->destination.version.resource == use.version.resource)
            {
                return PassNativeAccess{
                    fill->value ? nri::AccessBits::CLEAR_STORAGE : nri::AccessBits::COPY_DESTINATION,
                    fill->value ? nri::StageBits::CLEAR_STORAGE : zeroBufferStages(backend)
                };
            }
        }
        return std::nullopt;
    }
} // namespace lux::render::detail
