#pragma once
#include "lux/engine/gapi/vk/Image.hpp"

namespace lux::gapi::vk
{
    class ImageViewBuilder;
    class ImageView
    {
    public:
        using Builder = ImageViewBuilder;

        ImageView() : view(VK_NULL_HANDLE)
        {
        }

        ImageView(const VkImageViewCreateInfo& ci, VkDevice dev, VkAllocationCallbacks* allocator = nullptr)
        {
            VK_FUNC_INVOKE(vkCreateImageView, "Failed to create ImageView object", dev, &ci, allocator, &view);
        }

        ImageView(const ImageView&) = delete;
        ImageView& operator=(const ImageView&) = delete;

        ImageView(ImageView&& other) noexcept
        {
            view = other.view;
            other.view = VK_NULL_HANDLE;
        }

        ImageView& operator=(ImageView&& other) noexcept
        {
            view = other.view;
            other.view = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice dev, VkAllocationCallbacks* allocator = nullptr)
        {
            if (view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(dev, view, allocator);
                view = VK_NULL_HANDLE;
            }
        }

        inline operator VkImageView() const noexcept
        {
            return view;
        }
        inline const VkImageView* operator&() const noexcept
        {
            return &view;
        }

        inline VkImageView handle() const noexcept
        {
            return view;
        }
        inline const VkImageView* handlePtr() const noexcept
        {
            return &view;
        }

    private:
        VkImageView view;
    };

    class ImageViewBuilder
    {
    public:
        ImageViewBuilder()
        {
            create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            create_info.pNext = nullptr;
            create_info.flags = 0;
            create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create_info.format = VK_FORMAT_UNDEFINED;
            create_info.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY};
            create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }

        ImageViewBuilder& setFormat(VkFormat format)
        {
            create_info.format = format;
            return *this;
        }

        ImageViewBuilder& setComponents(const VkComponentMapping& components)
        {
            create_info.components = components;
            return *this;
        }

        ImageViewBuilder& setSubresourceRange(const VkImageSubresourceRange& range)
        {
            create_info.subresourceRange = range;
            return *this;
        }

        ImageViewBuilder& setSubresourceRange(
            VkImageAspectFlags aspect,
            uint32_t baseMipLevel,
            uint32_t levelCount,
            uint32_t baseArrayLayer,
            uint32_t layerCount
        )
        {
            create_info.subresourceRange.aspectMask = aspect;
            create_info.subresourceRange.baseMipLevel = baseMipLevel;
            create_info.subresourceRange.levelCount = levelCount;
            create_info.subresourceRange.baseArrayLayer = baseArrayLayer;
            create_info.subresourceRange.layerCount = layerCount;
            return *this;
        }

        ImageViewBuilder& setImage(VkImage image)
        {
            create_info.image = image;
            return *this;
        }

        ImageViewBuilder& setViewType(VkImageViewType type)
        {
            create_info.viewType = type;
            return *this;
        }

        ImageView build(VkDevice device) const
        {
            return ImageView(create_info, device);
        }

    private:
        VkImageViewCreateInfo create_info;
    };
}
