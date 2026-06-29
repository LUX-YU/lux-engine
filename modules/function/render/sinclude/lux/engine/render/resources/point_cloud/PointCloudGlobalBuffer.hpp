#pragma once
/**
 * @file PointCloudGlobalBuffer.hpp
 * @brief Single large GPU buffer serving as a unified SSBO/vertex buffer for
 *        all point cloud chunks (replaces per-leaf VkBuffer allocation).
 *
 * All point cloud rendering modes (GPU-Driven, LOD, Splatting, MeshShader)
 * share this buffer.  Switching render modes does NOT require re-uploading
 * any point data.
 *
 * Memory layout:
 *   [slot_0: first=0,     capacity=C0 ] GpuPointVertex[C0]
 *   [slot_1: first=C0,    capacity=C1 ] GpuPointVertex[C1]
 *   ...
 *
 * Allocation strategy: simple freelist over fixed-size slots.
 * Compaction is intentionally deferred to a dedicated compact() call so that
 * normal insert/delete paths are O(1).
 *
 * Thread safety: all methods must be called from the RENDER THREAD only.
 */

#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/render/resources/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/FrameRetireScheduler.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/SparseSet.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lux::render
{
    class TransferScheduler;

// ============================================================================
//  PointCloudGlobalBuffer
// ============================================================================

/**
 * @brief Unified GPU point buffer used by all multi-mode point cloud features.
 *
 * Usage:
 * @code
 *   PointCloudGlobalBuffer buf;
 *   buf.init(allocator, 4'000'000);   // reserve 4M point slots
 *
 *   // When a leaf becomes dirty (render thread):
 *   uint32_t cid = buf.allocSlot(leaf.alive_count);
 *   buf.upload(cid, packed_verts, upload_ctx);
 * @endcode
 */
class LUX_FUNCTION_PUBLIC PointCloudGlobalBuffer
{
public:
    // -----------------------------------------------------------------------
    //  Slot descriptor
    // -----------------------------------------------------------------------
    struct Slot
    {
        uint32_t first_point{0};   ///< Start index in the global point array
        uint32_t capacity{0};      ///< Allocated capacity (>= point_count)
        uint32_t point_count{0};   ///< Actual live point count (for draw calls)
    };

    PointCloudGlobalBuffer() = default;
    ~PointCloudGlobalBuffer() { shutdown(); }

    PointCloudGlobalBuffer(const PointCloudGlobalBuffer&)            = delete;
    PointCloudGlobalBuffer& operator=(const PointCloudGlobalBuffer&) = delete;
    PointCloudGlobalBuffer(PointCloudGlobalBuffer&&)                 = delete;
    PointCloudGlobalBuffer& operator=(PointCloudGlobalBuffer&&)      = delete;

    // -----------------------------------------------------------------------
    //  Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Allocate the backing GPU buffer.
     * @param allocator       VMA allocator (from ResourceContext::vmaAllocator())
     * @param max_points      Total point capacity of the buffer
     */
    bool init(VmaAllocator allocator, uint32_t max_points);
    void shutdown();

    void setDeferredQueue(DeferredDestroyQueue* q) noexcept { deferred_queue_ = q; }
    void setRetireScheduler(FrameRetireScheduler* rs) noexcept { retire_scheduler_ = rs; }
    void setRetireOwnerToken(FrameRetireScheduler::OwnerToken token) noexcept { retire_owner_token_ = token; }

    [[nodiscard]] bool isInitialized() const noexcept { return buffer_ != VK_NULL_HANDLE; }

    // -----------------------------------------------------------------------
    //  Slot management (render thread only)
    // -----------------------------------------------------------------------

    /**
     * @brief Reserve a slot for @p capacity points under a caller-supplied ID.
     *
     * The @p chunk_id must be unique and stable for the chunk's lifetime
     * (e.g. the octree NodeId or a gameplay-assigned sequential counter).
     *
     * @return true on success; false if the buffer is full or chunk_id already exists.
     */
    bool reserveSlot(uint32_t chunk_id, uint32_t capacity);

    /**
     * @brief Ensure @p chunk_id has a slot with at least @p capacity points.
     *
     * If the chunk already has enough capacity, this is a no-op. If it needs
     * to grow, a new region is allocated first and the old region is retired
     * after frames-in-flight delay, so in-flight draws remain valid.
     *
     * May trigger global-buffer growth when no free region can satisfy the
     * request. Growth is performed by allocating a larger buffer and issuing
     * a GPU copy from the previous buffer in @p ctx.
     */
    /// Growth copies are submitted with priority=-1; the scheduler's
    /// mid-barrier handles TRANSFER→TRANSFER sync.
    bool ensureSlotCapacity(uint32_t chunk_id,
                            uint32_t capacity,
                            TransferScheduler& scheduler);

    /**
     * @brief Schedule deferred release of a slot.
     *
     * The actual memory is not reclaimed until @p retire_frame +
     * @p frames_in_flight has passed (same pattern as GpuPointCloudManager).
     */
    void freeSlot(uint32_t chunk_id);

    /**
     * @brief Free all allocated slots, deferring each region's return to the free
     *        list by the current GPU serial.
     *
     * The slot map is cleared immediately, but — like freeSlot() — the regions are
     * not returned to the free list until the current serial's frame has retired.
     * This prevents a same-sweep clearAll-then-upload from re-allocating a region a
     * prior in-flight frame's draw is still reading (the backing buffer is GPU_ONLY
     * and persistent across frames).
     */
    void freeAllSlots();

    /**
     * @brief Reset a slot's point_count to 0 without releasing the allocation.
     *
     * The slot retains its GPU region and capacity so a subsequent uploadChunk
     * can reuse it without re-allocation. Complementary to removeChunk which
     * releases the slot entirely.
     */
    void resetSlot(uint32_t chunk_id) noexcept;

    /**
     * @brief Upload packed vertex data into an existing slot via staging.
     *
     * @param chunk_id    Target slot (must be allocated).
     * @param data        Tightly-packed GpuPointVertex array.
     * @param alloc       VMA allocator for staging-buffer creation.
     * @param cmd         VkCommandBuffer to record the copy into.
     * @param staging     Staging-buffer retirement list.
     * @return true on success; false if chunk_id is unknown or @p data is empty.
     */
    /// Staging allocation and copy recording are delegated to the scheduler.
    bool upload(uint32_t chunk_id,
                std::span<const GpuPointVertex> data,
                TransferScheduler& scheduler);

    /**
     * @brief Update only the live point count for a slot (no GPU copy).
     *
     * Used when alive flags change but no re-upload is needed (metadata stub path).
     */
    void setPointCount(uint32_t chunk_id, uint32_t point_count) noexcept;

    // -----------------------------------------------------------------------
    //  Queries
    // -----------------------------------------------------------------------

    [[nodiscard]] std::optional<Slot> getSlot(uint32_t chunk_id) const noexcept;

    [[nodiscard]] VkBuffer buffer()     const noexcept { return buffer_; }
    [[nodiscard]] uint32_t maxPoints()  const noexcept { return max_points_; }
    [[nodiscard]] uint32_t usedPoints() const noexcept;

    /// Whether a slot for this chunk_id has been reserved.
    [[nodiscard]] bool hasSlot(uint32_t chunk_id) const noexcept
    {
        return slots_.contains(chunk_id);
    }

    /**
     * @brief Iterate over all active slots.
     *
     * Callback signature: @code void fn(uint32_t chunk_id, const Slot& slot) @endcode
     *
     * Called from the render thread draw loop.
     * Slots whose point_count == 0 are included — callers should skip them.
     */
    template<typename Fn>
    void forEachSlot(Fn&& fn) const
    {
        const auto& keys = slots_.keys();
        const auto& values = slots_.values();
        const std::size_t count = std::min(keys.size(), values.size());
        for (std::size_t i = 0; i < count; ++i)
            fn(keys[i], values[i]);
    }

    static constexpr uint32_t kInvalidChunkId = ~uint32_t{0};

private:
    // -----------------------------------------------------------------------
    //  Free region management
    // -----------------------------------------------------------------------
    struct FreeRegion
    {
        uint32_t first;
        uint32_t capacity;
    };

    uint32_t tryAllocFromFreeList(uint32_t capacity);
    void     returnToFreeList(uint32_t first, uint32_t capacity);
    uint32_t allocOrGrow(uint32_t capacity, TransferScheduler& scheduler);
    bool     growBuffer(uint32_t required_capacity, TransferScheduler& scheduler);

    // -----------------------------------------------------------------------
    //  State
    // -----------------------------------------------------------------------
    VmaAllocator  allocator_{nullptr};
    VkBuffer      buffer_   {VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    uint32_t      max_points_{0};

    lux::cxx::OffsetSparseSet<uint32_t, Slot> slots_;    ///< chunk_id -> Slot
    std::vector<FreeRegion>               free_list_;    ///< sorted by first
    DeferredDestroyQueue*                 deferred_queue_{nullptr};
    FrameRetireScheduler*                 retire_scheduler_{nullptr};
    FrameRetireScheduler::OwnerToken      retire_owner_token_{0};
};

} // namespace lux::render
