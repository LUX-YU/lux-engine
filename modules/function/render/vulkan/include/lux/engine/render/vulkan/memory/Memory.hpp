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
            const VulkanAllocator& allocator,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access
        ) noexcept;
        ~Buffer() noexcept;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

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
        [[nodiscard]] VkDeviceSize allocationBytes() const noexcept;

    private:
        Buffer(
            VmaAllocator_T* allocator,
            VkDevice device,
            VkBuffer buffer,
            VmaAllocation_T* allocation,
            void* mapped,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access
        ) noexcept;
        void release() noexcept;

        VmaAllocator_T* allocator_;
        VkDevice device_;
        VkBuffer buffer_;
        VmaAllocation_T* allocation_;
        void* mapped_;
        VkDeviceSize size_;
        VkBufferUsageFlags usage_;
        EMemoryAccess access_;
    };

    struct ImageDescription
    {
        VkExtent2D extent;
        VkFormat format;
        VkImageUsageFlags usage;
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        std::uint32_t mip_levels{1};
        std::uint32_t array_layers{1};
        bool aliasable{false};
    };

    struct ImageMemoryBinding
    {
        VkDeviceMemory memory;
        VkDeviceSize offset, size;
        std::uint32_t memory_type;
        bool owns_allocation;
    };

    // Optimal tiled 2D backing. Views borrow it; array layers and mip levels
    // belong to this allocation, not to independent per-mip resource owners.
    class Image
    {
    public:
        [[nodiscard]] static RenderResult<Image> create(
            const VulkanAllocator& allocator,
            const ImageDescription& description
        ) noexcept;
        // Same native image shape, distinct VkImage, borrowed existing allocation.
        // The source allocation must outlive this alias. GPU non-overlap is the
        // caller's explicit synchronization obligation, not implied by this factory.
        [[nodiscard]] static RenderResult<Image> alias(const Image& source) noexcept;
        static RenderResult<Image> alias(const Image&&) = delete;
        [[nodiscard]] ImageMemoryBinding memoryBinding() const noexcept;
        [[nodiscard]] static RenderResult<Image> create(
            const VulkanAllocator& allocator,
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT
        ) noexcept;
        ~Image() noexcept;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

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
            return description_.extent;
        }

        [[nodiscard]] VkFormat format() const noexcept
        {
            return description_.format;
        }

        [[nodiscard]] VkImageUsageFlags usage() const noexcept
        {
            return description_.usage;
        }

        [[nodiscard]] VkSampleCountFlagBits samples() const noexcept
        {
            return description_.samples;
        }

        [[nodiscard]] const ImageDescription& description() const noexcept
        {
            return description_;
        }

    private:
        Image(
            VmaAllocator_T* allocator,
            VkDevice device,
            VkImage image,
            VmaAllocation_T* allocation,
            const ImageDescription& description
        ) noexcept;
        void release() noexcept;

        VmaAllocator_T* allocator_;
        VkDevice device_;
        VkImage image_;
        VmaAllocation_T* allocation_;
        ImageDescription description_;
        bool owns_allocation_{true};
    };
} // namespace lux::render::vulkan
