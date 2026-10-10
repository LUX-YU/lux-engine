#pragma once

#include <cstddef>
#include <lux/engine/render/vulkan/device/Device.hpp>

struct VmaAllocation_T;

namespace lux::render::vulkan
{
    enum class EMemoryAccess
    {
        DEVICE,
        UPLOAD,
        READBACK
    };

    class Buffer
    {
    public:
        [[nodiscard]] static RenderResult<Buffer> create(
            const VulkanAllocator &allocator, VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access
        ) noexcept;
        ~Buffer() noexcept;
        Buffer(Buffer &&other) noexcept;
        Buffer &operator=(Buffer &&other) noexcept;
        Buffer(const Buffer &) = delete;
        Buffer &operator=(const Buffer &) = delete;

        [[nodiscard]] VkBuffer native() const noexcept
        {
            return buffer_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] VkDeviceSize size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] VkBufferUsageFlags usage() const noexcept
        {
            return usage_;
        }

        // CPU/GPU access must not overlap. write flushes; read invalidates after
        // the caller has obtained GPU completion and a HOST_READ memory barrier.
        [[nodiscard]] RenderResult<void> write(VkDeviceSize offset, std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] RenderResult<void> read(VkDeviceSize offset, std::span<std::byte> bytes) const noexcept;

    private:
        Buffer(
            VmaAllocator_T *allocator,
            VkDevice device,
            VkBuffer buffer,
            VmaAllocation_T *allocation,
            void *mapped,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access
        ) noexcept;
        void release() noexcept;

        VmaAllocator_T *allocator_;
        VkDevice device_;
        VkBuffer buffer_;
        VmaAllocation_T *allocation_;
        void *mapped_;
        VkDeviceSize size_;
        VkBufferUsageFlags usage_;
        EMemoryAccess access_;
    };

    // R4 image primitive: optimal tiled 2D, one mip/layer/sample; no asset schema.
    class Image
    {
    public:
        [[nodiscard]] static RenderResult<Image> create(
            const VulkanAllocator &allocator, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage
        ) noexcept;
        ~Image() noexcept;
        Image(Image &&other) noexcept;
        Image &operator=(Image &&other) noexcept;
        Image(const Image &) = delete;
        Image &operator=(const Image &) = delete;

        [[nodiscard]] VkImage native() const noexcept
        {
            return image_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] VkExtent2D extent() const noexcept
        {
            return extent_;
        }

        [[nodiscard]] VkFormat format() const noexcept
        {
            return format_;
        }

        [[nodiscard]] VkImageUsageFlags usage() const noexcept
        {
            return usage_;
        }

    private:
        Image(
            VmaAllocator_T *allocator,
            VkDevice device,
            VkImage image,
            VmaAllocation_T *allocation,
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage
        ) noexcept;
        void release() noexcept;

        VmaAllocator_T *allocator_;
        VkDevice device_;
        VkImage image_;
        VmaAllocation_T *allocation_;
        VkExtent2D extent_;
        VkFormat format_;
        VkImageUsageFlags usage_;
    };
} // namespace lux::render::vulkan
