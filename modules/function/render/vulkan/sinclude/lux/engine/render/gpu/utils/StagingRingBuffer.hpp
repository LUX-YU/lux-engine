#pragma once
/**
 * @file StagingRingBuffer.hpp
 * @brief Ring-buffer sub-allocator for render-thread staging uploads.
 *
 * Owns a single large persistently-mapped VkBuffer.  Sub-allocations are
 * bump-pointer: advance head_, return {buffer, offset, mapped_ptr}.
 * Call reset() once the frame-in-flight fence has signalled to reclaim
 * the entire ring for the next frame.
 *
 * NOT thread-safe — intended for the render thread only (Path A staging).
 */

#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>

namespace lux::render
{

    struct StagingSubAlloc
    {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceSize offset{0};
        void* mapped{nullptr};

        explicit operator bool() const noexcept
        {
            return mapped != nullptr;
        }
    };

    class LUX_FUNCTION_PUBLIC StagingRingBuffer
    {
    public:
        StagingRingBuffer() = default;

        /// Create the ring buffer. Returns false on VMA allocation failure.
        /// @param frames_in_flight  Number of FIF slots; the ring is partitioned
        ///        so that each slot owns capacity/FIF bytes that never overlap.
        [[nodiscard]] bool init(VmaAllocator allocator, VkDeviceSize capacity, uint32_t frames_in_flight = 1);

        ~StagingRingBuffer();

        StagingRingBuffer(StagingRingBuffer&& o) noexcept;
        StagingRingBuffer& operator=(StagingRingBuffer&& o) noexcept;

        StagingRingBuffer(const StagingRingBuffer&) = delete;
        StagingRingBuffer& operator=(const StagingRingBuffer&) = delete;

        /// Try to sub-allocate @p bytes from the ring (aligned to @p alignment).
        /// Returns an empty StagingSubAlloc on overflow (caller should fall back
        /// to a dedicated VMA allocation).
        [[nodiscard]] StagingSubAlloc suballocate(VkDeviceSize bytes, VkDeviceSize alignment = 256);

        /// Reset the ring for the next frame.  Only safe to call after the GPU
        /// has finished all transfers that used the previous sub-allocations.
        void reset() noexcept;

        /// Reset the ring to the partition owned by @p frame_slot.
        /// Each slot gets capacity/frames_in_flight bytes of non-overlapping space.
        void resetSlot(uint32_t frame_slot) noexcept;

        [[nodiscard]] bool valid() const noexcept
        {
            return buffer_ != VK_NULL_HANDLE;
        }
        [[nodiscard]] VkDeviceSize capacity() const noexcept
        {
            return capacity_;
        }
        [[nodiscard]] VkDeviceSize used() const noexcept
        {
            return head_ - partition_start_;
        }

    private:
        void destroy() noexcept;

        VmaAllocator allocator_{nullptr};
        VkBuffer buffer_{VK_NULL_HANDLE};
        VmaAllocation allocation_{nullptr};
        void* base_ptr_{nullptr};
        VkDeviceSize capacity_{0};
        VkDeviceSize head_{0};
        VkDeviceSize partition_start_{0};
        VkDeviceSize partition_end_{0};
        uint32_t frames_in_flight_{1};
    };

} // namespace lux::render
