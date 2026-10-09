#pragma once

#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace lux::render
{
    /// Complete host-mapped vertex storage. The engine frame index selects the
    /// slot. Runtime detach transfers its owners into the existing FIF retire
    /// sink; ordinary destruction is only safe before submission or at idle.
    class TransientVertexRing final
    {
    public:
        struct Slot
        {
            VmaBuffer buffer;
            void* mapped{};
        };

        [[nodiscard]] static Expected<TransientVertexRing> create(
            VmaAllocator allocator,
            std::uint32_t frames_in_flight,
            VkDeviceSize slot_bytes
        ) noexcept
        {
            const auto count = std::max<std::uint32_t>(1u, frames_in_flight);
            std::vector<Slot> slots;
            slots.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                info.size = slot_bytes;
                info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                VmaAllocationCreateInfo allocation_info{};
                allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
                allocation_info.flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VkBuffer buffer{};
                VmaAllocation allocation{};
                VmaAllocationInfo mapping{};
                const auto result = vmaCreateBuffer(allocator, &info, &allocation_info, &buffer, &allocation, &mapping);
                if (result != VK_SUCCESS)
                {
                    return renderFailure<err::memory::GpuAllocationFailed>();
                }
                auto owner = VmaBuffer::adopt({allocator, buffer, allocation});
                if (!mapping.pMappedData)
                {
                    return renderFailure<err::memory::GpuAllocationFailed>();
                }
                slots.push_back({std::move(owner), mapping.pMappedData});
            }
            return TransientVertexRing(allocator, std::move(slots));
        }

        TransientVertexRing(const TransientVertexRing&) = delete;
        TransientVertexRing& operator=(const TransientVertexRing&) = delete;
        TransientVertexRing(TransientVertexRing&&) noexcept = default;
        TransientVertexRing& operator=(TransientVertexRing&&) noexcept = default;
        ~TransientVertexRing() noexcept = default;

        [[nodiscard]] std::uint32_t size() const noexcept
        {
            return static_cast<std::uint32_t>(slots_.size());
        }

        [[nodiscard]] std::uint32_t slotIndexFor(std::uint32_t frame_index) const noexcept
        {
            return frame_index % size();
        }

        [[nodiscard]] const Slot& slotAt(std::uint32_t index) const noexcept
        {
            return slots_[index];
        }

        void flush(std::uint32_t index, VkDeviceSize bytes) noexcept
        {
            vmaFlushAllocation(allocator_, slots_[index].buffer.allocation(), 0, bytes);
        }

        /// Consumes storage into the existing synchronous retirement sink.
        /// A consumed/moved-from ring may only be destroyed or reassigned.
        template <class RetireFn> void retireInto(RetireFn&& retire) && noexcept
        {
            for (auto& slot : slots_)
            {
                const auto allocation = slot.buffer.release();
                retire(allocation.buffer, allocation.allocation);
            }
            slots_.clear();
        }

    private:
        TransientVertexRing(VmaAllocator allocator, std::vector<Slot> slots) noexcept
            : allocator_(allocator), slots_(std::move(slots))
        {
        }

        VmaAllocator allocator_;
        std::vector<Slot> slots_;
    };
} // namespace lux::render
