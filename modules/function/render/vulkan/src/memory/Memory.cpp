#include <lux/engine/render/vulkan/memory/Memory.hpp>

#include "Native.hpp"
#include <cstring>
#include <utility>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    Buffer::Buffer(
        VmaAllocator_T* allocator,
        VkDevice device,
        VkBuffer buffer,
        VmaAllocation_T* allocation,
        void* mapped,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        EMemoryAccess access
    ) noexcept
        : allocator_(allocator), device_(device), buffer_(buffer), allocation_(allocation), mapped_(mapped),
          size_(size), usage_(usage), access_(access)
    {
    }

    RenderResult<Buffer> Buffer::create(
        const VulkanAllocator& allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        EMemoryAccess access
    ) noexcept
    {
        const bool is_invalid_access =
            access != EMemoryAccess::DEVICE && access != EMemoryAccess::UPLOAD && access != EMemoryAccess::READBACK;
        const bool is_invalid_config = size == 0 || usage == 0 || is_invalid_access;
        if (is_invalid_config)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        if (access == EMemoryAccess::DEVICE)
        {
            memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        else
        {
            memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                           (access == EMemoryAccess::UPLOAD ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
            memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo mapped{};
        const auto result =
            LUX_NATIVE("buffer", vmaCreateBuffer(allocator.native(), &info, &memory, &buffer, &allocation, &mapped));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        Buffer
            owner{allocator.native(), allocator.device(), buffer, allocation, mapped.pMappedData, size, usage, access};
        if (access != EMemoryAccess::DEVICE)
        {
            const auto mapping = LUX_NATIVE("mapping", mapped.pMappedData ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED);
            if (mapping != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(mapping));
            }
        }
        return owner;
    }

    void Buffer::release() noexcept
    {
        if (buffer_)
        {
            LUX_DESTROY("buffer", vmaDestroyBuffer(allocator_, buffer_, allocation_));
        }
    }

    Buffer::~Buffer() noexcept
    {
        release();
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : allocator_(other.allocator_), device_(other.device_), buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
          allocation_(other.allocation_), mapped_(other.mapped_), size_(other.size_), usage_(other.usage_),
          access_(other.access_)
    {
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            release();
            allocator_ = other.allocator_;
            device_ = other.device_;
            buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
            allocation_ = other.allocation_;
            mapped_ = other.mapped_;
            size_ = other.size_;
            usage_ = other.usage_;
            access_ = other.access_;
        }
        return *this;
    }

    RenderResult<void> Buffer::write(VkDeviceSize offset, std::span<const std::byte> bytes) noexcept
    {
        const bool is_invalid_range = offset > size_ || bytes.size() > size_ - offset;
        const bool is_invalid_access = access_ == EMemoryAccess::DEVICE || is_invalid_range;
        if (is_invalid_access)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (bytes.empty())
        {
            return {};
        }
        std::memcpy(static_cast<std::byte*>(mapped_) + offset, bytes.data(), bytes.size());
        const auto result = LUX_NATIVE("flush", vmaFlushAllocation(allocator_, allocation_, offset, bytes.size()));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return {};
    }

    RenderResult<void> Buffer::read(VkDeviceSize offset, std::span<std::byte> bytes) const noexcept
    {
        const bool is_invalid_range = offset > size_ || bytes.size() > size_ - offset;
        const bool is_invalid_access = access_ != EMemoryAccess::READBACK || is_invalid_range;
        if (is_invalid_access)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (bytes.empty())
        {
            return {};
        }
        const auto result =
            LUX_NATIVE("invalidate", vmaInvalidateAllocation(allocator_, allocation_, offset, bytes.size()));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        std::memcpy(bytes.data(), static_cast<const std::byte*>(mapped_) + offset, bytes.size());
        return {};
    }

    Image::Image(
        VmaAllocator_T* allocator,
        VkDevice device,
        VkImage image,
        VmaAllocation_T* allocation,
        VkExtent2D extent,
        VkFormat format,
        VkImageUsageFlags usage,
        VkSampleCountFlagBits samples
    ) noexcept
        : allocator_(allocator), device_(device), image_(image), allocation_(allocation), extent_(extent),
          format_(format), usage_(usage), samples_(samples)
    {
    }

    RenderResult<Image> Image::create(
        const VulkanAllocator& allocator,
        VkExtent2D extent,
        VkFormat format,
        VkImageUsageFlags usage,
        VkSampleCountFlagBits samples
    ) noexcept
    {
        const bool is_invalid_config =
            extent.width == 0 || extent.height == 0 || format == VK_FORMAT_UNDEFINED || usage == 0 || samples == 0 ||
            (static_cast<std::uint32_t>(samples) & (static_cast<std::uint32_t>(samples) - 1)) != 0;
        if (is_invalid_config)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VmaAllocatorInfo allocator_info{};
        vmaGetAllocatorInfo(allocator.native(), &allocator_info);
        VkImageFormatProperties supported{};
        const auto support = vkGetPhysicalDeviceImageFormatProperties(
            allocator_info.physicalDevice,
            format,
            VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            usage,
            0,
            &supported
        );
        if (support != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(support));
        }
        const bool is_too_large =
            extent.width > supported.maxExtent.width || extent.height > supported.maxExtent.height;
        if (is_too_large || (supported.sampleCounts & samples) == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VkImage image{};
        VmaAllocation allocation{};
        const auto result =
            LUX_NATIVE("image", vmaCreateImage(allocator.native(), &info, &memory, &image, &allocation, nullptr));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return Image{allocator.native(), allocator.device(), image, allocation, extent, format, usage, samples};
    }

    void Image::release() noexcept
    {
        if (image_)
        {
            LUX_DESTROY("image", vmaDestroyImage(allocator_, image_, allocation_));
        }
    }

    Image::~Image() noexcept
    {
        release();
    }

    Image::Image(Image&& other) noexcept
        : allocator_(other.allocator_), device_(other.device_), image_(std::exchange(other.image_, VK_NULL_HANDLE)),
          allocation_(other.allocation_), extent_(other.extent_), format_(other.format_), usage_(other.usage_),
          samples_(other.samples_)
    {
    }

    Image& Image::operator=(Image&& other) noexcept
    {
        if (this != &other)
        {
            release();
            allocator_ = other.allocator_;
            device_ = other.device_;
            image_ = std::exchange(other.image_, VK_NULL_HANDLE);
            allocation_ = other.allocation_;
            extent_ = other.extent_;
            format_ = other.format_;
            usage_ = other.usage_;
            samples_ = other.samples_;
        }
        return *this;
    }
} // namespace lux::render::vulkan
