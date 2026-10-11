#include <lux/engine/render/vulkan/descriptor/ImageBindings.hpp>

#include "Native.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::render::vulkan
{
    RenderResult<ImageView> ImageView::create(const Image& image, VkImageAspectFlags aspect) noexcept
    {
        return create(image, VkImageSubresourceRange{aspect, 0, 1, 0, 1});
    }

    RenderResult<ImageView> ImageView::create(
        const Image& image,
        VkImageSubresourceRange range,
        VkImageViewType type
    ) noexcept
    {
        const auto aspect = range.aspectMask;
        VkImageAspectFlags allowed = VK_IMAGE_ASPECT_COLOR_BIT;
        switch (image.format())
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            allowed = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        case VK_FORMAT_S8_UINT:
            allowed = VK_IMAGE_ASPECT_STENCIL_BIT;
            break;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            allowed = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            break;
        default:
            break;
        }
        const bool invalid_aspect = aspect == 0 || (aspect & ~allowed) != 0;
        const auto& desc = image.description();
        const bool invalid_range = range.baseMipLevel >= desc.mip_levels || range.levelCount == 0 ||
                                   range.levelCount > desc.mip_levels - range.baseMipLevel ||
                                   range.baseArrayLayer >= desc.array_layers || range.layerCount == 0 ||
                                   range.layerCount > desc.array_layers - range.baseArrayLayer;
        const bool invalid_type = (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
                                  (type == VK_IMAGE_VIEW_TYPE_2D && range.layerCount != 1);
        if (invalid_aspect || invalid_range || invalid_type || !image.native())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image.native();
        info.viewType = type;
        info.format = image.format();
        info.subresourceRange = range;
        VkImageView view{};
        const auto result = LUX_NATIVE("image_view", vkCreateImageView(image.device(), &info, nullptr, &view));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return ImageView{image, view, range, type};
    }

    ImageView::ImageView(
        const Image& image,
        VkImageView handle,
        VkImageSubresourceRange range,
        VkImageViewType type
    ) noexcept
        : device_(image.device()), handle_(handle), usage_(image.usage()), format_(image.format()),
          extent_{
              std::max(1u, image.extent().width >> range.baseMipLevel),
              std::max(1u, image.extent().height >> range.baseMipLevel)
          },
          range_(range), type_(type), image_(image.native()), description_(image.description()),
          samples_(image.samples())
    {
    }

    void ImageView::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("image_view", vkDestroyImageView(device_, handle_, nullptr));
        }
    }

    ImageView::~ImageView() noexcept
    {
        release();
    }

    ImageView::ImageView(ImageView&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE)), usage_(other.usage_),
          format_(other.format_), extent_(other.extent_), range_(other.range_), type_(other.type_),
          image_(other.image_), description_(other.description_), samples_(other.samples_)
    {
    }

    ImageView& ImageView::operator=(ImageView&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
            usage_ = other.usage_;
            format_ = other.format_;
            extent_ = other.extent_;
            range_ = other.range_;
            type_ = other.type_;
            image_ = other.image_;
            description_ = other.description_;
            samples_ = other.samples_;
        }
        return *this;
    }

    RenderResult<Sampler> Sampler::create(const VulkanDevice& device, const SamplerDescription& description) noexcept
    {
        const bool invalid_filter =
            description.min_filter < VK_FILTER_NEAREST || description.min_filter > VK_FILTER_LINEAR ||
            description.mag_filter < VK_FILTER_NEAREST || description.mag_filter > VK_FILTER_LINEAR ||
            description.mipmap < VK_SAMPLER_MIPMAP_MODE_NEAREST || description.mipmap > VK_SAMPLER_MIPMAP_MODE_LINEAR;
        const bool invalid_address =
            description.u < VK_SAMPLER_ADDRESS_MODE_REPEAT || description.u > VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
            description.v < VK_SAMPLER_ADDRESS_MODE_REPEAT || description.v > VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
            description.w < VK_SAMPLER_ADDRESS_MODE_REPEAT || description.w > VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        const bool invalid_lod = !std::isfinite(description.min_lod) || !std::isfinite(description.max_lod) ||
                                 description.min_lod < 0 || description.max_lod < description.min_lod;
        if (invalid_filter || invalid_address || invalid_lod)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.minFilter = description.min_filter;
        info.magFilter = description.mag_filter;
        info.mipmapMode = description.mipmap;
        info.addressModeU = description.u;
        info.addressModeV = description.v;
        info.addressModeW = description.w;
        info.minLod = description.min_lod;
        info.maxLod = description.max_lod;
        VkSampler sampler{};
        const auto result = LUX_NATIVE("sampler", vkCreateSampler(device.native(), &info, nullptr, &sampler));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return Sampler{device.native(), sampler};
    }

    Sampler::Sampler(VkDevice device, VkSampler handle) noexcept : device_(device), handle_(handle) {}

    void Sampler::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("sampler", vkDestroySampler(device_, handle_, nullptr));
        }
    }

    Sampler::~Sampler() noexcept
    {
        release();
    }

    Sampler::Sampler(Sampler&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }

    Sampler& Sampler::operator=(Sampler&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }
} // namespace lux::render::vulkan
