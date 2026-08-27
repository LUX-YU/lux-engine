#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{

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
