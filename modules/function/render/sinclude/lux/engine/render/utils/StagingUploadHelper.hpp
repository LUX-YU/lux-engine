#pragma once
/**
 * @file StagingUploadHelper.hpp
 * @brief Shared staging-buffer allocation helper for render-thread uploads.
 *
 * Provides a small struct (StagingAlloc) and a free function (allocStaging)
 * used by any GPU_ONLY resource that needs CPU→GPU staging copies on the
 * render thread (PointCloudGlobalBuffer, GpuOctreeNodeBuffer, etc.).
 *
 * Pattern mirrors the anonymous-namespace helper in InstanceResources.cpp.
 */

#include <lux/engine/render/resources/memory/StagingBuffer.hpp>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/function/visibility.h>

#include <vector>

namespace lux::render
{

class StagingRingBuffer;

/// Result of a transient staging-buffer allocation (host-visible, mapped).
struct StagingAlloc
{
    VkBuffer      buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    void*         mapped{nullptr};
    VkDeviceSize  srcOffset{0};    ///< Offset within buffer (non-zero for ring sub-allocs).

    explicit operator bool() const noexcept { return mapped != nullptr; }
};

/**
 * @brief Allocate a host-visible staging buffer via VMA.
 *
 * The resulting StagingBuffer RAII wrapper is appended to @p batch so the
 * caller can retire it after the GPU copy has completed (e.g. at frame end).
 *
 * @param allocator   VMA allocator
 * @param bytes       Required staging size in bytes
 * @param batch       Staging buffer retirement list (ownership transferred)
 * @return StagingAlloc with mapped pointer, or empty on failure
 */
LUX_FUNCTION_PUBLIC StagingAlloc allocStaging(VmaAllocator allocator, VkDeviceSize bytes,
                                              std::vector<StagingBuffer>& batch);

/**
 * @brief Ring-buffer-aware staging allocation (preferred fast path).
 *
 * Tries to sub-allocate from @p ring first.  On overflow, falls back to a
 * dedicated VMA allocation appended to @p batch.
 * The returned StagingAlloc::srcOffset must be used as VkBufferCopy::srcOffset.
 */
LUX_FUNCTION_PUBLIC StagingAlloc allocStaging(VmaAllocator allocator, VkDeviceSize bytes,
                                              std::vector<StagingBuffer>& batch,
                                              StagingRingBuffer& ring);

} // namespace lux::render
