#pragma once

#include <cstddef>
#include <exception>
#include <lux/engine/render/native/vulkan/device/Device.hpp>
#include <memory>
#include <utility>

struct VmaAllocation_T;

namespace lux::render::vulkan
{
    enum class EMemoryAccess
    {
        DEVICE,
        UPLOAD,
        READBACK
    };

    struct BufferMemoryBinding
    {
        VkDeviceMemory memory;
        VkDeviceSize offset, size;
    };

    // Owns one allocation; moving transfers it without changing Native identity.
    // Borrowed GPU uses must end before destruction or move assignment; the allocator outlives this value.
    class Buffer
    {
    public:
        Buffer() noexcept = default;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        [[nodiscard]] VkBuffer native() const noexcept
        {
            return buffer_;
        }

        [[nodiscard]] VkDeviceSize size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] EMemoryAccess access() const noexcept
        {
            return access_;
        }

        [[nodiscard]] VkBufferUsageFlags usage() const noexcept
        {
            return usage_;
        }

        ~Buffer() noexcept;

        [[nodiscard]] RenderResult<void> write(VkDeviceSize offset, std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] RenderResult<void> read(VkDeviceSize offset, std::span<std::byte> bytes) const noexcept;
        [[nodiscard]] BufferMemoryBinding memoryBinding() const noexcept;

        [[nodiscard]] std::span<std::byte> mappedBytes() noexcept
        {
            return mapped_ ? std::span(static_cast<std::byte*>(mapped_), static_cast<std::size_t>(size_))
                           : std::span<std::byte>{};
        }

        [[nodiscard]] RenderResult<void> flush(VkDeviceSize offset, VkDeviceSize size) noexcept;

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

    private:
        friend class VulkanAllocator;
        friend class Transfer;

        Buffer(
            VulkanAllocator& allocator,
            VkBuffer buffer,
            VmaAllocation_T* allocation,
            void* mapped,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access
        ) noexcept
            : allocator_(&allocator), buffer_(buffer), allocation_(allocation), mapped_(mapped), size_(size),
              usage_(usage), access_(access)
        {
        }

        void release() noexcept;
        VulkanAllocator* allocator_ = nullptr;
        VkBuffer buffer_ = VK_NULL_HANDLE;
        VmaAllocation_T* allocation_ = nullptr;
        void* mapped_ = nullptr;
        VkDeviceSize size_ = 0;
        VkBufferUsageFlags usage_ = 0;
        EMemoryAccess access_ = EMemoryAccess::DEVICE;
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
    };

    // Actual VMA allocation shared by alias Images. No Image points at another Image.
    class ImageMemory final
    {
    public:
        ImageMemory(VulkanAllocator& allocator, VmaAllocation_T* allocation) noexcept
            : allocator_(&allocator), allocation_(allocation)
        {
        }

        ~ImageMemory() noexcept;
        ImageMemory(const ImageMemory&) = delete;
        ImageMemory& operator=(const ImageMemory&) = delete;

    private:
        friend class VulkanAllocator;
        friend class Image;
        VulkanAllocator* allocator_;
        VmaAllocation_T* allocation_;
    };

    // Optimal tiled 2D backing. Views borrow it; array layers and mip levels
    // belong to this allocation, not to independent per-mip resource owners.
    class Image
    {
    public:
        Image() noexcept = default;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;

        [[nodiscard]] VkImage native() const noexcept
        {
            return image_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return memory_ ? memory_->allocator_->device() : VK_NULL_HANDLE;
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

        ~Image() noexcept;

        [[nodiscard]] ImageMemoryBinding memoryBinding() const noexcept;

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

    private:
        friend class VulkanAllocator;

        Image(VkImage image, std::shared_ptr<ImageMemory> memory, ImageDescription description) noexcept
            : image_(image), memory_(std::move(memory)), description_(description)
        {
        }

        void release() noexcept;
        VkImage image_ = VK_NULL_HANDLE;
        std::shared_ptr<ImageMemory> memory_;
        ImageDescription description_{};
    };
} // namespace lux::render::vulkan
