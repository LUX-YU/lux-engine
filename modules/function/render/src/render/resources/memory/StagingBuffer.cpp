#include <lux/engine/render/resources/memory/StagingBuffer.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{

StagingBuffer::StagingBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) noexcept
    : allocator_(allocator)
    , buffer_(buffer)
    , allocation_(allocation)
{
}

StagingBuffer::~StagingBuffer()
{
    reset();
}

StagingBuffer::StagingBuffer(StagingBuffer&& o) noexcept
    : allocator_(o.allocator_)
    , buffer_(o.buffer_)
    , allocation_(o.allocation_)
{
    o.allocator_  = VK_NULL_HANDLE;
    o.buffer_     = VK_NULL_HANDLE;
    o.allocation_ = VK_NULL_HANDLE;
}

StagingBuffer& StagingBuffer::operator=(StagingBuffer&& o) noexcept
{
    if (this != &o)
    {
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

void StagingBuffer::reset() noexcept
{
    if (buffer_ != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    allocator_  = VK_NULL_HANDLE;
    buffer_     = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
}

} // namespace lux::render

