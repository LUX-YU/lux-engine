#include <lux/engine/render/utils/StagingRingBuffer.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace lux::render
{
    bool StagingRingBuffer::init(VmaAllocator allocator, VkDeviceSize capacity,
                                   uint32_t frames_in_flight)
    {
        destroy();

        if (frames_in_flight == 0) frames_in_flight = 1;

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = capacity;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        if (vmaCreateBuffer(allocator, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS)
            return false;

        allocator_ = allocator;
        buffer_ = buf;
        allocation_ = alloc;
        base_ptr_ = info.pMappedData;
        capacity_ = capacity;
        frames_in_flight_ = frames_in_flight;
        partition_start_ = 0;
        partition_end_   = capacity / frames_in_flight;
        head_ = 0;
        return true;
    }

    StagingRingBuffer::~StagingRingBuffer()
    {
        destroy();
    }

    StagingRingBuffer::StagingRingBuffer(StagingRingBuffer &&o) noexcept
        : allocator_(o.allocator_), buffer_(o.buffer_), allocation_(o.allocation_), base_ptr_(o.base_ptr_), capacity_(o.capacity_), head_(o.head_), partition_start_(o.partition_start_), partition_end_(o.partition_end_), frames_in_flight_(o.frames_in_flight_)
    {
        o.allocator_ = nullptr;
        o.buffer_ = VK_NULL_HANDLE;
        o.allocation_ = nullptr;
        o.base_ptr_ = nullptr;
        o.capacity_ = 0;
        o.head_ = 0;
        o.partition_start_ = 0;
        o.partition_end_ = 0;
        o.frames_in_flight_ = 1;
    }

    StagingRingBuffer &StagingRingBuffer::operator=(StagingRingBuffer &&o) noexcept
    {
        if (this != &o)
        {
            destroy();
            allocator_ = o.allocator_;
            buffer_ = o.buffer_;
            allocation_ = o.allocation_;
            base_ptr_ = o.base_ptr_;
            capacity_ = o.capacity_;
            head_ = o.head_;
            partition_start_ = o.partition_start_;
            partition_end_ = o.partition_end_;
            frames_in_flight_ = o.frames_in_flight_;
            o.allocator_ = nullptr;
            o.buffer_ = VK_NULL_HANDLE;
            o.allocation_ = nullptr;
            o.base_ptr_ = nullptr;
            o.capacity_ = 0;
            o.head_ = 0;
            o.partition_start_ = 0;
            o.partition_end_ = 0;
            o.frames_in_flight_ = 1;
        }
        return *this;
    }

    StagingSubAlloc StagingRingBuffer::suballocate(VkDeviceSize bytes, VkDeviceSize alignment)
    {
        if (buffer_ == VK_NULL_HANDLE || bytes == 0)
            return {};

        // Align head_ up to the requested alignment.
        VkDeviceSize aligned_head = (head_ + alignment - 1) & ~(alignment - 1);
        if (aligned_head + bytes > partition_end_)
            return {}; // overflow — caller falls back to dedicated alloc

        void *ptr = static_cast<std::byte *>(base_ptr_) + aligned_head;
        head_ = aligned_head + bytes;
        return {buffer_, aligned_head, ptr};
    }

    void StagingRingBuffer::reset() noexcept
    {
        head_ = partition_start_;
    }

    void StagingRingBuffer::resetSlot(uint32_t frame_slot) noexcept
    {
        const VkDeviceSize part_size = capacity_ / frames_in_flight_;
        const uint32_t slot = frame_slot % frames_in_flight_;
        partition_start_ = part_size * slot;
        partition_end_   = partition_start_ + part_size;
        head_ = partition_start_;
    }

    void StagingRingBuffer::destroy() noexcept
    {
        if (buffer_ != VK_NULL_HANDLE && allocator_ != nullptr)
        {
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
        }
        allocator_ = nullptr;
        buffer_ = VK_NULL_HANDLE;
        allocation_ = nullptr;
        base_ptr_ = nullptr;
        capacity_ = 0;
        head_ = 0;
        partition_start_ = 0;
        partition_end_ = 0;
        frames_in_flight_ = 1;
    }

} // namespace lux::render
