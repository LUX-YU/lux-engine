#include "GraphNative.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>

namespace lux::render::vulkan::detail
{
    namespace
    {
        struct CopyBlock
        {
            std::uint32_t width, bytes;
        };

        // Vulkan buffer/image packing, including the single-aspect depth/stencil
        // representations. This is not a second format identity or classification.
        CopyBlock copyBlock(rdesc::ETextureFormat format, EAspect aspect) noexcept
        {
            using F = rdesc::ETextureFormat;
            if (aspect == EAspect::STENCIL)
            {
                return {1, 1};
            }
            switch (format)
            {
            case F::R8_UNORM:
            case F::R8_SNORM:
            case F::R8_UINT:
            case F::R8_SINT:
            case F::R8_SRGB:
                return {1, 1};
            case F::R16_UNORM:
            case F::R16_SNORM:
            case F::R16_UINT:
            case F::R16_SINT:
            case F::R16_SFLOAT:
            case F::RG8_UNORM:
            case F::RG8_SNORM:
            case F::RG8_UINT:
            case F::RG8_SINT:
            case F::RG8_SRGB:
            case F::D16_UNORM:
            case F::D16_UNORM_S8_UINT:
                return {1, 2};
            case F::RGB8_UNORM:
            case F::RGB8_SNORM:
            case F::RGB8_UINT:
            case F::RGB8_SINT:
            case F::RGB8_SRGB:
                return {1, 3};
            case F::R32_UINT:
            case F::R32_SINT:
            case F::R32_SFLOAT:
            case F::RG16_UNORM:
            case F::RG16_SNORM:
            case F::RG16_UINT:
            case F::RG16_SINT:
            case F::RG16_SFLOAT:
            case F::RGBA8_UNORM:
            case F::RGBA8_SNORM:
            case F::RGBA8_UINT:
            case F::RGBA8_SINT:
            case F::RGBA8_SRGB:
            case F::BGRA8_UNORM:
            case F::BGRA8_SRGB:
            case F::D32_SFLOAT:
            case F::D24_UNORM_S8_UINT:
            case F::D32_SFLOAT_S8_UINT:
                return {1, 4};
            case F::RG32_UINT:
            case F::RG32_SINT:
            case F::RG32_SFLOAT:
            case F::RGBA16_UNORM:
            case F::RGBA16_SNORM:
            case F::RGBA16_UINT:
            case F::RGBA16_SINT:
            case F::RGBA16_SFLOAT:
                return {1, 8};
            case F::RGB32_UINT:
            case F::RGB32_SINT:
            case F::RGB32_SFLOAT:
                return {1, 12};
            case F::RGBA32_UINT:
            case F::RGBA32_SINT:
            case F::RGBA32_SFLOAT:
                return {1, 16};
            case F::BC1_RGB_UNORM:
            case F::BC1_RGB_SRGB:
            case F::BC1_RGBA_UNORM:
            case F::BC1_RGBA_SRGB:
            case F::BC4_UNORM:
            case F::BC4_SNORM:
                return {4, 8};
            case F::BC2_UNORM:
            case F::BC2_SRGB:
            case F::BC3_UNORM:
            case F::BC3_SRGB:
            case F::BC5_UNORM:
            case F::BC5_SNORM:
            case F::BC6H_UFLOAT:
            case F::BC6H_SFLOAT:
            case F::BC7_UNORM:
            case F::BC7_SRGB:
                return {4, 16};
            default:
                return {};
            }
        }

        RenderResult<CopyRecipe> bufferImage(
            const GraphResource& image,
            ImageRange range,
            BufferRange bytes,
            std::uint32_t source,
            std::optional<std::uint32_t> destination
        ) noexcept
        {
            const auto block = copyBlock(image.texture().format, range.aspect);
            const bool invalid = block.bytes == 0 || image.texture().samples != 1 ||
                                 !std::has_single_bit(static_cast<unsigned>(range.aspect));
            if (invalid)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto alignment = std::lcm(4u, block.bytes);
            if (bytes.byte_offset % alignment != 0)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            std::vector<VkBufferImageCopy> regions;
            VkDeviceSize cursor = bytes.byte_offset;
            for (std::uint32_t m = 0; m < range.mip_count; ++m)
            {
                const auto mip = range.base_mip + m;
                const auto width = std::max(1u, image.texture().width >> mip);
                const auto height = std::max(1u, image.texture().height >> mip);
                const auto size = VkDeviceSize{(width + block.width - 1) / block.width} *
                                  ((height + block.width - 1) / block.width) * range.layer_count * block.bytes;
                cursor += (alignment - cursor % alignment) % alignment;
                const bool overflow = cursor < bytes.byte_offset || cursor - bytes.byte_offset > bytes.byte_count;
                if (overflow || size > bytes.byte_count - (cursor - bytes.byte_offset))
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                regions.push_back(
                    {cursor,
                     0,
                     0,
                     {static_cast<VkImageAspectFlags>(range.aspect), mip, range.base_layer, range.layer_count},
                     {},
                     {width, height, 1}}
                );
                cursor += size;
            }
            return CopyRecipe{source, destination, std::move(regions), cursor - bytes.byte_offset, alignment};
        }
    } // namespace

    RenderResult<void> compileCopies(NativePassRecipe& recipe, const LogicalGraphPlan& plan) noexcept
    {
        const auto p = recipe.command.pass.value() - 1;
        const auto& pass = plan.identity().passes[p];
        const auto count = plan.cacheIdentity().passes[p].uses.size();
        const bool host = pass.kind == EPassKind::HOST_READBACK;
        if (!host && count != 2)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (std::uint32_t u = 0; u < count; ++u)
        {
            const auto& source = pass.uses[u];
            if (source.access != EGraphAccess::READ)
            {
                continue;
            }
            const auto destination = host ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{1 - u};
            if (destination && pass.uses[*destination].access != EGraphAccess::WRITE)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& image_or_buffer = plan.identity().resources[source.resource.value() - 1];
            const auto* from_image = std::get_if<ImageRange>(&source.range);
            const auto* to_image = destination ? std::get_if<ImageRange>(&pass.uses[*destination].range) : nullptr;
            if (from_image && to_image)
            {
                const auto& target = plan.identity().resources[pass.uses[*destination].resource.value() - 1];
                const bool incompatible = image_or_buffer.texture().format != target.texture().format ||
                                          image_or_buffer.texture().samples != target.texture().samples ||
                                          from_image->mip_count != to_image->mip_count ||
                                          from_image->layer_count != to_image->layer_count ||
                                          from_image->aspect != to_image->aspect ||
                                          !std::has_single_bit(static_cast<unsigned>(from_image->aspect));
                if (incompatible)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                std::vector<VkImageCopy> regions;
                for (std::uint32_t m = 0; m < from_image->mip_count; ++m)
                {
                    const auto a = from_image->base_mip + m, b = to_image->base_mip + m;
                    const auto width = std::max(1u, image_or_buffer.texture().width >> a);
                    const auto height = std::max(1u, image_or_buffer.texture().height >> a);
                    if (width != std::max(1u, target.texture().width >> b) ||
                        height != std::max(1u, target.texture().height >> b))
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    regions.push_back(
                        {{static_cast<VkImageAspectFlags>(from_image->aspect),
                          a,
                          from_image->base_layer,
                          from_image->layer_count},
                         {},
                         {static_cast<VkImageAspectFlags>(to_image->aspect),
                          b,
                          to_image->base_layer,
                          to_image->layer_count},
                         {},
                         {width, height, 1}}
                    );
                }
                recipe.copies.push_back({u, destination, std::move(regions), 0});
            }
            else if (from_image || to_image)
            {
                const auto& image = from_image
                                        ? image_or_buffer
                                        : plan.identity().resources[pass.uses[*destination].resource.value() - 1];
                const auto bytes =
                    !destination ? BufferRange{0, std::numeric_limits<std::uint64_t>::max()}
                                 : std::get<BufferRange>(from_image ? pass.uses[*destination].range : source.range);
                auto copy = bufferImage(image, from_image ? *from_image : *to_image, bytes, u, destination);
                if (!copy)
                {
                    return cxx::unexpected(copy.error());
                }
                recipe.copies.push_back(std::move(*copy));
            }
            else
            {
                const auto a = std::get<BufferRange>(source.range);
                const auto b =
                    destination ? std::get<BufferRange>(pass.uses[*destination].range) : BufferRange{0, a.byte_count};
                const bool invalid = a.byte_offset % 4 != 0 || b.byte_offset % 4 != 0 || a.byte_count % 4 != 0 ||
                                     a.byte_count != b.byte_count;
                if (invalid)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                recipe.copies.push_back(
                    {u, destination, VkBufferCopy{a.byte_offset, b.byte_offset, a.byte_count}, a.byte_count}
                );
            }
        }
        if (recipe.copies.empty())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        return {};
    }

    void recordCopies(
        VkCommandBuffer command,
        const NativePassRecipe& recipe,
        const GraphSlot& slot,
        const FrameGraphBindings& bindings
    ) noexcept
    {
        for (std::size_t c = 0; c < recipe.copies.size(); ++c)
        {
            const auto& copy = recipe.copies[c];
            const auto a = bindings.resourceFor(recipe.command.pass, copy.source_use).value() - 1;
            const auto b =
                copy.destination_use ? bindings.resourceFor(recipe.command.pass, *copy.destination_use).value() - 1 : 0;
            const auto* from_image = imageOf(slot.resources[a]);
            const auto* from_buffer = bufferOf(slot.resources[a]);
            const auto* to_image = copy.destination_use ? imageOf(slot.resources[b]) : nullptr;
            const auto* to_buffer = copy.destination_use ? bufferOf(slot.resources[b])
                                                         : &slot.passes[recipe.command.pass.value() - 1].readbacks[c];
            const auto destination_offset = copy.destination_use ? slot.buffer_offsets[b] : 0;
            if (const auto* buffer = std::get_if<VkBufferCopy>(&copy.regions))
            {
                auto region = *buffer;
                region.srcOffset += slot.buffer_offsets[a];
                region.dstOffset += destination_offset;
                vkCmdCopyBuffer(command, from_buffer->native(), to_buffer->native(), 1, &region);
            }
            else if (const auto* images = std::get_if<std::vector<VkImageCopy>>(&copy.regions))
            {
                vkCmdCopyImage(
                    command,
                    from_image->native(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    to_image->native(),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<std::uint32_t>(images->size()),
                    images->data()
                );
            }
            else
            {
                for (auto region : std::get<std::vector<VkBufferImageCopy>>(copy.regions))
                {
                    if (from_image)
                    {
                        region.bufferOffset += destination_offset;
                        vkCmdCopyImageToBuffer(
                            command,
                            from_image->native(),
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            to_buffer->native(),
                            1,
                            &region
                        );
                    }
                    else
                    {
                        region.bufferOffset += slot.buffer_offsets[a];
                        vkCmdCopyBufferToImage(
                            command,
                            from_buffer->native(),
                            to_image->native(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1,
                            &region
                        );
                    }
                }
            }
            if (!copy.destination_use)
            {
                VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = to_buffer->native();
                barrier.size = copy.bytes;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(command, &dependency);
            }
        }
    }
} // namespace lux::render::vulkan::detail
