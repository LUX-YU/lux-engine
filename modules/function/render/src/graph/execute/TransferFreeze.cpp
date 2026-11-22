#include <lux/engine/render/graph/execute/CommandExtent.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>

namespace lux::render::detail
{
    namespace
    {
        bool containsBytes(const PassResourceInput& resource, std::uint64_t offset, std::uint64_t count) noexcept
        {
            const bool fits = offset <= resource.buffer_size && count <= resource.buffer_size - offset;
            const bool addressable = offset <= UINT64_MAX - resource.buffer_offset &&
                count <= UINT64_MAX - (resource.buffer_offset + offset);
            return resource.buffer && fits && addressable;
        }

        RenderResult<void> freezeCompute(
            const ComputeCommands& packet, std::span<const PassResourceInput> resources,
            const nri::DeviceDesc& device, PassNativeInput& native
        ) noexcept
        {
            if (const auto* indirect = std::get_if<BufferLocation>(&packet.dispatch))
            {
                const auto& buffer = resources[indirect->buffer];
                if (!containsBytes(buffer, indirect->offset, sizeof(nri::DispatchDesc)))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                native.indirect_buffer = buffer.buffer;
                native.indirect_offset = buffer.buffer_offset + indirect->offset;
                if (native.indirect_offset % 4) return cxx::unexpected(RenderError{kDeviceArgument});
            }
            else if (const auto* fixed = std::get_if<nri::DispatchDesc>(&packet.dispatch)) native.dispatch = *fixed;
            else
            {
                const auto& extent = std::get<ImageDispatchExtent>(packet.dispatch);
                const auto& image = resources[extent.image];
                auto groups = dispatchGroups(extent, {image.width, image.height}, device);
                if (!groups) return cxx::unexpected(groups.error());
                native.dispatch = *groups;
            }
            return {};
        }

        struct TransferFreeze final
        {
            std::span<const PassResourceInput> resources;
            std::span<nri::Descriptor* const> private_views, history_views, clear_views;
            CopyAlignment alignment;

            const PassResourceInput& selected(const auto& operand) const noexcept
            {
                const auto& primary = resources[operand.slot];
                const bool use_fallback = !primary.history_valid && operand.fallback != kGraphInvalidIndex;
                return use_fallback ? resources[operand.fallback] : primary;
            }

            RenderResult<nri::TextureRegionDesc> region(const ImageOperand& operand) const noexcept
            {
                const auto& image = selected(operand);
                if (!image.image) return cxx::unexpected(RenderError{kDeviceArgument});
                return copyRegion(operand, {image.width, image.height});
            }

            RenderResult<VFrozenTransfer> operator()(const BufferCopyPacket& copy) const noexcept
            {
                const auto& source = selected(copy.source);
                const auto& destination = selected(copy.destination);
                const auto [offset, size] = copy.source.range;
                const auto to = copy.destination.range.offset;
                const bool fits = containsBytes(source, offset, size) && containsBytes(destination, to, size);
                if (!fits)
                    return cxx::unexpected(RenderError{kDeviceArgument});
                return VFrozenTransfer{BufferCopyRegion{
                    source.buffer, destination.buffer,
                    source.buffer_offset + offset, destination.buffer_offset + to, size
                }};
            }

            RenderResult<VFrozenTransfer> operator()(const ImageCopyPacket& copy) const noexcept
            {
                auto from = region(copy.source);
                if (!from) return cxx::unexpected(from.error());
                auto to = region(copy.destination);
                if (!to) return cxx::unexpected(to.error());
                if (from->width != to->width || from->height != to->height)
                    return cxx::unexpected(RenderError{kInvalidTargetExtent});
                return VFrozenTransfer{TextureCopyRegion{
                    selected(copy.source).image, selected(copy.destination).image, *from, *to,
                    copy.source.range.mip_count, copy.source.range.layer_count
                }};
            }

            RenderResult<VFrozenTransfer> operator()(const ImageBufferPacket& copy) const noexcept
            {
                auto rect = region(copy.image);
                if (!rect) return cxx::unexpected(rect.error());
                const auto& buffer = selected(copy.buffer);
                auto layout = copyLayout(copy, *rect, buffer.buffer_offset, alignment);
                if (!layout) return cxx::unexpected(layout.error());
                if (!containsBytes(buffer, copy.buffer.range.offset, layout->slicePitch))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                auto& image = *selected(copy.image).image;
                if (copy.direction == EImageBufferCopy::READBACK)
                    return VFrozenTransfer{ReadbackRegion{*buffer.buffer, *layout, image, *rect}};
                return VFrozenTransfer{UploadRegion{image, *rect, *buffer.buffer, *layout}};
            }

            RenderResult<VFrozenTransfer> operator()(const FillPacket& fill) const noexcept
            {
                const auto& buffer = selected(fill.destination);
                const auto [offset, size] = fill.destination.range;
                const bool aligned = (buffer.buffer_offset + offset) % 4 == 0;
                if (!aligned || !containsBytes(buffer, offset, size))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                nri::Descriptor* view = nullptr;
                if (fill.descriptor != kGraphInvalidIndex)
                {
                    view = private_views[fill.descriptor];
                    if (!view) view = history_views[fill.descriptor];
                    if (!view) return cxx::unexpected(RenderError{kDeviceArgument});
                }
                return VFrozenTransfer{FrozenBufferFill{
                    buffer.buffer, buffer.buffer_offset + offset, size, view,
                    fill.descriptor, fill.value, fill.sync_before
                }};
            }

            RenderResult<VFrozenTransfer> operator()(const ClearPacket& clear) const noexcept
            {
                FrozenImageClear frozen{};
                frozen.attachment.descriptor = clear_views[clear.view];
                if (!frozen.attachment.descriptor) return cxx::unexpected(RenderError{kDeviceArgument});
                frozen.attachment.loadOp = nri::LoadOp::CLEAR;
                frozen.attachment.storeOp = nri::StoreOp::STORE;
                frozen.depth = clear.destination.range.aspects != 1;
                if (frozen.depth) frozen.attachment.clearValue.depthStencil.depth = clear.depth;
                else frozen.attachment.clearValue.color.f = {
                    clear.color[0], clear.color[1], clear.color[2], clear.color[3]
                };
                return VFrozenTransfer{frozen};
            }
        };
    }

    RenderResult<void> RenderGraphInstance::Pass::freezeCommands(
        const compiled::RenderGraph& plan, std::span<const PassResourceInput> resources,
        const nri::DeviceDesc& device, PassNativeInput& native
    ) noexcept
    {
        if (const auto* compute = plan.computeCommands(*compiled_pass))
            return freezeCompute(*compute, resources, device, native);
        const auto* transfers = plan.transferCommands(*compiled_pass);
        if (!transfers) return {};
        const auto& memory = device.memoryAlignment;
        const TransferFreeze freeze{
            resources, private_views, history_views, clear_views,
            {memory.uploadBufferTextureRow, memory.uploadBufferTextureSlice}
        };
        frozen_transfers.clear();
        for (const auto& packet : transfers->operations)
        {
            auto result = std::visit(freeze, packet);
            if (!result) return cxx::unexpected(result.error());
            frozen_transfers.push_back(std::move(*result));
        }
        native.transfers = frozen_transfers;
        return {};
    }
}
