#include <lux/engine/render/gpu/utils/StagingRingBuffer.hpp>
#include <vk_mem_alloc.h>

#include <bit>
#include <cstddef>
#include <utility>

namespace lux::render
{
    Expected<StagingRingBuffer> StagingRingBuffer::create(
        VmaAllocator allocator,
        VkDeviceSize capacity,
        uint32_t frames_in_flight
    ) noexcept
    {
        const bool has_allocator = allocator != nullptr;
        const bool has_partitions = frames_in_flight != 0 && capacity >= frames_in_flight;
        const bool is_invalid_config = !has_allocator || !has_partitions;
        if (is_invalid_config)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = capacity;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        // Callers write returned spans directly; no later noncoherent flush protocol exists.
        allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo mapping{};
        VkBuffer buffer{};
        VmaAllocation allocation{};
        const auto status = vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &buffer, &allocation, &mapping);
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        StagingBuffer backing(allocator, buffer, allocation);
        if (!mapping.pMappedData)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
        }
        return StagingRingBuffer(std::move(backing), mapping.pMappedData, capacity, frames_in_flight);
    }

    StagingRingBuffer::StagingRingBuffer(
        StagingBuffer backing,
        void* mapped,
        VkDeviceSize capacity,
        uint32_t frames
    ) noexcept
        : backing_(std::move(backing)), base_ptr_(mapped), capacity_(capacity), partition_end_(capacity / frames),
          frames_in_flight_(frames)
    {
    }

    StagingSubAlloc StagingRingBuffer::suballocate(VkDeviceSize bytes, VkDeviceSize alignment)
    {
        const bool has_backing = backing_.valid();
        const bool is_invalid_request = bytes == 0 || !std::has_single_bit(alignment);
        if (!has_backing || is_invalid_request)
        {
            return {};
        }
        const auto padding = (alignment - (head_ & (alignment - 1))) & (alignment - 1);
        const auto available = partition_end_ - head_;
        const bool exceeds_partition = padding > available || bytes > available - padding;
        if (exceeds_partition)
        {
            return {};
        }
        const auto aligned_head = head_ + padding;
        head_ = aligned_head + bytes;
        return {backing_.buffer(), aligned_head, static_cast<std::byte*>(base_ptr_) + aligned_head};
    }

    void StagingRingBuffer::reset() noexcept
    {
        head_ = partition_start_;
    }

    void StagingRingBuffer::resetSlot(uint32_t frame_slot) noexcept
    {
        const auto part_size = capacity_ / frames_in_flight_;
        const auto slot = frame_slot % frames_in_flight_;
        partition_start_ = part_size * slot;
        partition_end_ = partition_start_ + part_size;
        head_ = partition_start_;
    }
} // namespace lux::render
