#pragma once
/**
 * @file StagingBuffer.hpp
 * @brief Move-only RAII wrapper for a VMA-allocated staging buffer.
 *
 * Follows the same ownership pattern as VmaBuffer but is intentionally
 * minimal: it wraps an already-created VkBuffer + VmaAllocation pair
 * and destroys it in the destructor.  No map/unmap helpers — callers
 * manage mapping through VMA directly.
 *
 * Use case: deferred retirement of staging buffers across frame-in-flight
 * boundaries.  Replacing raw {VkBuffer, VmaAllocation} PODs with this
 * type ensures automatic cleanup on all code paths (exceptions, early
 * returns, container clear()).
 */
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/VmaFwd.hpp>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC StagingBuffer
    {
    public:
        StagingBuffer() = default;

        /// Adopt ownership of an existing VMA-allocated staging buffer.
        StagingBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) noexcept;

        ~StagingBuffer();

        StagingBuffer(StagingBuffer&& o) noexcept;

        StagingBuffer& operator=(StagingBuffer&& o) noexcept;

        StagingBuffer(const StagingBuffer&)            = delete;
        StagingBuffer& operator=(const StagingBuffer&) = delete;

        [[nodiscard]] VkBuffer      buffer()     const noexcept { return buffer_; }
        [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
        [[nodiscard]] bool          valid()      const noexcept { return buffer_ != VK_NULL_HANDLE; }

        /// Explicitly release the allocation (idempotent).
        void reset() noexcept;

    private:
        VmaAllocator  allocator_  {VK_NULL_HANDLE};
        VkBuffer      buffer_     {VK_NULL_HANDLE};
        VmaAllocation allocation_ {VK_NULL_HANDLE};
    };

} // namespace lux::render
