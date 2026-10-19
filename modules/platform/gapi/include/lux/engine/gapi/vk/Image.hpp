#pragma once
#include "lux/engine/gapi/vk/DeviceMemory.hpp"
#include "lux/engine/gapi/vk/LogicalDevice.hpp"
#include <lux/cxx/compile_time/expected.hpp>
#include <memory>
#include <utility>
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class ImageBuilder;

    class Image
    {
    public:
        using Builder = ImageBuilder;

        Image() noexcept : image(VK_NULL_HANDLE) {}

        Image(VkImage image) : image(image) {}

        [[nodiscard]] static cxx::expected<Image, VkResult> create(
            VkDevice device,
            const VkImageCreateInfo& info,
            const VkAllocationCallbacks* allocator = nullptr
        ) noexcept
        {
            Image value;
            const auto status = vkCreateImage(device, &info, allocator, &value.image);
            if (status != VK_SUCCESS)
            {
                return cxx::unexpected(status);
            }
            return value;
        }

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        Image(Image&& other) noexcept
        {
            image = other.image;
            other.image = VK_NULL_HANDLE;
        }

        Image& operator=(Image&& other) noexcept
        {
            if (this != std::addressof(other))
            {
                // The collection owner releases an occupied destination before assignment.
                image = std::exchange(other.image, VK_NULL_HANDLE);
            }
            return *this;
        }

        void release(VkDevice dev, const VkAllocationCallbacks* allocator = nullptr) noexcept
        {
            if (image != VK_NULL_HANDLE)
            {
                vkDestroyImage(dev, image, allocator);
                image = VK_NULL_HANDLE;
            }
        }

        VkMemoryRequirements memoryRequirements(VkDevice dev) const noexcept
        {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(dev, image, &req);
            return req;
        }

        DeviceMemory allocateMemory(
            VkDevice dev,
            VkDeviceSize allocation_size,
            uint32_t memory_type_index,
            VkAllocationCallbacks* allocator = nullptr
        )
        {
            return DeviceMemory{dev, allocation_size, memory_type_index, allocator};
        }

        [[nodiscard]] VkResult bindMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept
        {
            return vkBindImageMemory(device, image, memory, offset);
        }

        inline operator bool() const noexcept
        {
            return image != VK_NULL_HANDLE;
        }

        inline bool operator!() const noexcept
        {
            return image == VK_NULL_HANDLE;
        }

        inline operator VkImage() const noexcept
        {
            return image;
        }

        inline const VkImage* operator&() const noexcept
        {
            return &image;
        }

        inline VkImage handle() const noexcept
        {
            return image;
        }

        inline const VkImage* handlePtr() const noexcept
        {
            return &image;
        }

    private:
        VkImage image;
    };

    class ImageBuilder
    {
    public:
        ImageBuilder() noexcept
        {
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        ImageBuilder& setFlags(VkImageCreateFlags flags)
        {
            ci.flags = flags;
            return *this;
        }

        ImageBuilder& setImageType(VkImageType type)
        {
            ci.imageType = type;
            return *this;
        }

        ImageBuilder& setFormat(VkFormat format)
        {
            ci.format = format;
            return *this;
        }

        ImageBuilder& setExtent(VkExtent3D extent)
        {
            ci.extent = extent;
            return *this;
        }

        ImageBuilder& setMipLevels(uint32_t mipLevels)
        {
            ci.mipLevels = mipLevels;
            return *this;
        }

        ImageBuilder& setArrayLayers(uint32_t arrayLayers)
        {
            ci.arrayLayers = arrayLayers;
            return *this;
        }

        ImageBuilder& setSamples(VkSampleCountFlagBits samples)
        {
            ci.samples = samples;
            return *this;
        }

        ImageBuilder& setTiling(VkImageTiling tiling)
        {
            ci.tiling = tiling;
            return *this;
        }

        ImageBuilder& setUsage(VkImageUsageFlags usage)
        {
            ci.usage = usage;
            return *this;
        }

        ImageBuilder& setSharingMode(VkSharingMode sharingMode)
        {
            ci.sharingMode = sharingMode;
            return *this;
        }

        ImageBuilder& setQueueFamilyIndices(const uint32_t* indices, uint32_t count)
        {
            ci.queueFamilyIndexCount = count;
            ci.pQueueFamilyIndices = indices;
            return *this;
        }

        ImageBuilder& setInitialLayout(VkImageLayout layout)
        {
            ci.initialLayout = layout;
            return *this;
        }

        ImageBuilder& setExternalMemoryHandleTypes(VkExternalMemoryHandleTypeFlags types) noexcept
        {
            external_types_ = types;
            return *this;
        }

        [[nodiscard]] cxx::expected<Image, VkResult> build(
            VkDevice device,
            const VkAllocationCallbacks* allocator = nullptr
        ) const noexcept
        {
            auto create_info = ci;
            VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            if (external_types_)
            {
                external.handleTypes = external_types_;
                external.pNext = create_info.pNext;
                create_info.pNext = &external;
            }
            return Image::create(device, create_info, allocator);
        }

        [[nodiscard]] cxx::expected<Image, VkResult> buildAndBind(
            VkDevice device,
            VkDeviceMemory memory,
            VkDeviceSize offset,
            const VkAllocationCallbacks* allocator = nullptr
        ) const noexcept
        {
            auto image = build(device, allocator);
            if (!image)
            {
                return cxx::unexpected(image.error());
            }
            const auto status = image->bindMemory(device, memory, offset);
            if (status != VK_SUCCESS)
            {
                image->release(device, allocator);
                return cxx::unexpected(status);
            }
            return image;
        }

    private:
        static uint32_t calculateAlignedOffset(uint32_t currentOffset, uint32_t alignment)
        {
            uint32_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
            return alignedOffset;
        }

        VkImageCreateInfo ci{};
        VkExternalMemoryHandleTypeFlags external_types_ = 0;
    };
} // namespace lux::gapi::vk
