#include <lux/engine/render/resources/memory/VmaTypes.hpp>
#include <vk_mem_alloc.h>
#include <stdexcept>

namespace lux::render
{

// =====================================================================
//  VmaBuffer
// =====================================================================

VmaBuffer::VmaBuffer(VmaAllocator alloc,
                     const VkBufferCreateInfo& bci,
                     const VmaAllocationCreateInfo& aci)
    : allocator_(alloc)
{
    VkResult res = vmaCreateBuffer(alloc, &bci, &aci, &buffer_, &allocation_, nullptr);
    if (res != VK_SUCCESS)
        throw std::runtime_error("VmaBuffer: vmaCreateBuffer failed");
}

VmaBuffer::~VmaBuffer()
{
    reset();
}

VmaBuffer::VmaBuffer(VmaBuffer&& o) noexcept
    : allocator_(o.allocator_)
    , buffer_(o.buffer_)
    , allocation_(o.allocation_)
{
    o.allocator_  = VK_NULL_HANDLE;
    o.buffer_     = VK_NULL_HANDLE;
    o.allocation_ = VK_NULL_HANDLE;
}

VmaBuffer& VmaBuffer::operator=(VmaBuffer&& o) noexcept
{
    if (this != &o) {
        reset();
        allocator_  = o.allocator_;
        buffer_     = o.buffer_;
        allocation_ = o.allocation_;
        o.allocator_  = VK_NULL_HANDLE;
        o.buffer_     = VK_NULL_HANDLE;
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
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        buffer_     = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

// =====================================================================
//  VmaImage
// =====================================================================

VmaImage::VmaImage(VmaAllocator alloc, const VkImageCreateInfo& ici, const VmaAllocationCreateInfo& aci)
    : allocator_(alloc)
{
    VkResult res = vmaCreateImage(alloc, &ici, &aci, &image_, &allocation_, nullptr);
    if (res != VK_SUCCESS)
        throw std::runtime_error("VmaImage: vmaCreateImage failed");
}

VmaImage::~VmaImage()
{
    reset();
}

VmaImage::VmaImage(VmaImage&& o) noexcept
    : allocator_(o.allocator_)
    , image_(o.image_)
    , allocation_(o.allocation_)
{
    o.allocator_  = VK_NULL_HANDLE;
    o.image_      = VK_NULL_HANDLE;
    o.allocation_ = VK_NULL_HANDLE;
}

VmaImage& VmaImage::operator=(VmaImage&& o) noexcept
{
    if (this != &o) {
        reset();
        allocator_  = o.allocator_;
        image_      = o.image_;
        allocation_ = o.allocation_;
        o.allocator_  = VK_NULL_HANDLE;
        o.image_      = VK_NULL_HANDLE;
        o.allocation_ = VK_NULL_HANDLE;
    }
    return *this;
}

void VmaImage::reset() noexcept
{
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
        image_      = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

} // namespace lux::render
