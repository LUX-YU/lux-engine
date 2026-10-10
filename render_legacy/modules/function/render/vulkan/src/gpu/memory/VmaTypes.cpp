#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <vk_mem_alloc.h>
#include <utility>

namespace lux::render
{
    VmaAllocatorOwner::CreateResult VmaAllocatorOwner::create(const VmaAllocatorCreateInfo& info) noexcept
    {
        VmaAllocator allocator{};
        const auto result = vmaCreateAllocator(&info, &allocator);
        if (result != VK_SUCCESS)
        {
            return lux::cxx::unexpected(result);
        }
        return VmaAllocatorOwner(allocator);
    }

    VmaAllocatorOwner::~VmaAllocatorOwner() noexcept
    {
        reset();
    }

    VmaAllocatorOwner::VmaAllocatorOwner(VmaAllocatorOwner&& other) noexcept
        : allocator_(std::exchange(other.allocator_, nullptr))
    {
    }

    VmaAllocatorOwner& VmaAllocatorOwner::operator=(VmaAllocatorOwner&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            allocator_ = std::exchange(other.allocator_, nullptr);
        }
        return *this;
    }

    void VmaAllocatorOwner::reset() noexcept
    {
        if (allocator_)
        {
            vmaDestroyAllocator(std::exchange(allocator_, nullptr));
        }
    }

    VmaPoolOwner::CreateResult VmaPoolOwner::create(VmaAllocator allocator, const VmaPoolCreateInfo& info) noexcept
    {
        VmaPool pool{};
        const auto result = vmaCreatePool(allocator, &info, &pool);
        if (result != VK_SUCCESS)
        {
            return lux::cxx::unexpected(result);
        }
        return VmaPoolOwner(allocator, pool);
    }

    VmaPoolOwner::~VmaPoolOwner() noexcept
    {
        reset();
    }

    VmaPoolOwner::VmaPoolOwner(VmaPoolOwner&& other) noexcept
        : allocator_(std::exchange(other.allocator_, nullptr)), pool_(std::exchange(other.pool_, nullptr))
    {
    }

    VmaPoolOwner& VmaPoolOwner::operator=(VmaPoolOwner&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            allocator_ = std::exchange(other.allocator_, nullptr);
            pool_ = std::exchange(other.pool_, nullptr);
        }
        return *this;
    }

    void VmaPoolOwner::reset() noexcept
    {
        if (pool_)
        {
            vmaDestroyPool(allocator_, std::exchange(pool_, nullptr));
            allocator_ = nullptr;
        }
    }

    // =====================================================================
    //  VmaBuffer
    // =====================================================================

    Expected<VmaBuffer> VmaBuffer::create(
        VmaAllocator allocator,
        const VkBufferCreateInfo& buffer_info,
        const VmaAllocationCreateInfo& allocation_info
    )
    {
        VmaBuffer result;
        result.allocator_ = allocator;
        const VkResult status =
            vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &result.buffer_, &result.allocation_, nullptr);
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        return result;
    }

    VmaBuffer::~VmaBuffer()
    {
        reset();
    }

    VmaBuffer::VmaBuffer(VmaBuffer&& o) noexcept
        : allocator_(o.allocator_), buffer_(o.buffer_), allocation_(o.allocation_)
    {
        o.allocator_ = VK_NULL_HANDLE;
        o.buffer_ = VK_NULL_HANDLE;
        o.allocation_ = VK_NULL_HANDLE;
    }

    VmaBuffer& VmaBuffer::operator=(VmaBuffer&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            allocator_ = o.allocator_;
            buffer_ = o.buffer_;
            allocation_ = o.allocation_;
            o.allocator_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    void* VmaBuffer::map()
    {
        void* mapped = nullptr;
        if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
            return nullptr;
        return mapped;
    }

    void VmaBuffer::unmap()
    {
        vmaUnmapMemory(allocator_, allocation_);
    }

    void VmaBuffer::flush(VkDeviceSize offset, VkDeviceSize size)
    {
        vmaFlushAllocation(allocator_, allocation_, offset, size);
    }

    void VmaBuffer::reset() noexcept
    {
        if (buffer_ != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
            buffer_ = VK_NULL_HANDLE;
            allocation_ = VK_NULL_HANDLE;
        }
    }

    // =====================================================================
    //  VmaImage
    // =====================================================================

    Expected<VmaImage> VmaImage::create(
        VmaAllocator allocator,
        const VkImageCreateInfo& image_info,
        const VmaAllocationCreateInfo& allocation_info
    )
    {
        VmaImage result;
        result.allocator_ = allocator;
        const VkResult status =
            vmaCreateImage(allocator, &image_info, &allocation_info, &result.image_, &result.allocation_, nullptr);
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        return result;
    }

    VmaImage::~VmaImage()
    {
        reset();
    }

    VmaImage::VmaImage(VmaImage&& o) noexcept : allocator_(o.allocator_), image_(o.image_), allocation_(o.allocation_)
    {
        o.allocator_ = VK_NULL_HANDLE;
        o.image_ = VK_NULL_HANDLE;
        o.allocation_ = VK_NULL_HANDLE;
    }

    VmaImage& VmaImage::operator=(VmaImage&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            allocator_ = o.allocator_;
            image_ = o.image_;
            allocation_ = o.allocation_;
            o.allocator_ = VK_NULL_HANDLE;
            o.image_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    void VmaImage::reset() noexcept
    {
        if (image_ != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_, image_, allocation_);
            image_ = VK_NULL_HANDLE;
            allocation_ = VK_NULL_HANDLE;
        }
    }

} // namespace lux::render
