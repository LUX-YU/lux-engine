#pragma once

#include <lux/engine/render/vulkan/memory/Memory.hpp>

namespace lux::render::vulkan
{
    // Owns only VkImageView; image backing and device are explicit borrows.
    class ImageView
    {
    public:
        [[nodiscard]] static RenderResult<ImageView> create(const Image& image, VkImageAspectFlags aspect) noexcept;
        static RenderResult<ImageView> create(const Image&&, VkImageAspectFlags) = delete;
        [[nodiscard]] static RenderResult<ImageView> create(
            const Image& image,
            VkImageSubresourceRange range,
            VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D
        ) noexcept;
        static RenderResult<ImageView> create(
            const Image&&,
            VkImageSubresourceRange,
            VkImageViewType = VK_IMAGE_VIEW_TYPE_2D
        ) = delete;
        ~ImageView() noexcept;
        ImageView(ImageView&& other) noexcept;
        ImageView& operator=(ImageView&& other) noexcept;
        ImageView(const ImageView&) = delete;
        ImageView& operator=(const ImageView&) = delete;

        [[nodiscard]] VkImageView native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] VkImageUsageFlags usage() const noexcept
        {
            return usage_;
        }

        [[nodiscard]] VkFormat format() const noexcept
        {
            return format_;
        }

        [[nodiscard]] VkExtent2D extent() const noexcept
        {
            return extent_;
        }

        [[nodiscard]] VkImageAspectFlags aspect() const noexcept
        {
            return range_.aspectMask;
        }

        [[nodiscard]] const VkImageSubresourceRange& range() const noexcept
        {
            return range_;
        }

        [[nodiscard]] VkImageViewType type() const noexcept
        {
            return type_;
        }

        [[nodiscard]] VkImage image() const noexcept
        {
            return image_;
        }

        [[nodiscard]] const ImageDescription& backingDescription() const noexcept
        {
            return description_;
        }

        [[nodiscard]] VkSampleCountFlagBits samples() const noexcept
        {
            return samples_;
        }

    private:
        ImageView(const Image& image, VkImageView handle, VkImageSubresourceRange range, VkImageViewType type) noexcept;
        void release() noexcept;
        VkDevice device_;
        VkImageView handle_;
        VkImageUsageFlags usage_;
        VkFormat format_;
        VkExtent2D extent_;
        VkImageSubresourceRange range_;
        VkImageViewType type_;
        VkImage image_;
        ImageDescription description_;
        VkSampleCountFlagBits samples_;
    };

    struct SamplerDescription
    {
        VkFilter min_filter{VK_FILTER_NEAREST}, mag_filter{VK_FILTER_NEAREST};
        VkSamplerMipmapMode mipmap{VK_SAMPLER_MIPMAP_MODE_NEAREST};
        VkSamplerAddressMode u{VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE}, v{VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
            w{VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        float min_lod{}, max_lod{};
        bool operator==(const SamplerDescription&) const noexcept = default;
    };

    class Sampler
    {
    public:
        [[nodiscard]] static RenderResult<Sampler> create(
            const VulkanDevice& device,
            const SamplerDescription& description = {}
        ) noexcept;
        ~Sampler() noexcept;
        Sampler(Sampler&& other) noexcept;
        Sampler& operator=(Sampler&& other) noexcept;
        Sampler(const Sampler&) = delete;
        Sampler& operator=(const Sampler&) = delete;

        [[nodiscard]] VkSampler native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        Sampler(VkDevice device, VkSampler handle) noexcept;
        void release() noexcept;
        VkDevice device_;
        VkSampler handle_;
    };
} // namespace lux::render::vulkan
