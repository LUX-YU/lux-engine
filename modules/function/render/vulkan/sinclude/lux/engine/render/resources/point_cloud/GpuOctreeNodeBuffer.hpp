#pragma once
/**
 * @file GpuOctreeNodeBuffer.hpp
 * @brief GPU-side SSBO managing an array of GpuOctreeNode structs.
 *
 * Used by the GPU-Driven (mode 2) and LOD (mode 3) rendering features to
 * supply per-node metadata (bounding box, first_point, point_count, lod_error,
 * etc.) to the compute shader for frustum culling and LOD selection.
 *
 * Layout: a single GPU_ONLY VkBuffer (STORAGE_BUFFER | TRANSFER_DST) that
 * stores GpuOctreeNode[max_nodes] in a packed array.  Node positions in the
 * array are stable for the lifetime of a chunk and reclaimed via a freelist
 * when chunks are deleted.
 *
 * Thread safety: all methods must be called from the RENDER THREAD only.
 *
 * @see GpuOctreeNode (PointCloudGpuData.hpp)
 * @see PointCloudGlobalBuffer
 */

#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/SparseSet.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace lux::render
{
    class TransferScheduler;

// ============================================================================
//  GpuOctreeNodeBuffer
// ============================================================================

class LUX_FUNCTION_PUBLIC GpuOctreeNodeBuffer
{
public:
    GpuOctreeNodeBuffer() = default;
    ~GpuOctreeNodeBuffer() { shutdown(); }

    GpuOctreeNodeBuffer(const GpuOctreeNodeBuffer&)            = delete;
    GpuOctreeNodeBuffer& operator=(const GpuOctreeNodeBuffer&) = delete;
    GpuOctreeNodeBuffer(GpuOctreeNodeBuffer&&)                 = delete;
    GpuOctreeNodeBuffer& operator=(GpuOctreeNodeBuffer&&)      = delete;

    // -----------------------------------------------------------------------
    //  Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Allocate the backing SSBO.
     * @param allocator   VMA allocator
     * @param max_nodes   Maximum number of nodes the buffer can hold
     */
    bool init(VmaAllocator allocator, uint32_t max_nodes);
    void shutdown();

    void setDeferredQueue(DeferredDestroyQueue* q) noexcept { deferred_queue_ = q; }

    /// Byte size of the SSBO header: { uint node_count; uint padding[3]; }
    /// Nodes are stored at kHeaderSize + index * sizeof(GpuOctreeNode).
    static constexpr VkDeviceSize kHeaderSize = 16;

    [[nodiscard]] bool     isInitialized() const noexcept { return buffer_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkBuffer buffer()        const noexcept { return buffer_; }
    [[nodiscard]] uint32_t nodeCount()     const noexcept { return static_cast<uint32_t>(chunk_to_index_.size()); }
    [[nodiscard]] uint32_t maxNodes()      const noexcept { return max_nodes_; }
    /// High-water mark of allocated SSBO slots (= node_count written to GPU header).
    [[nodiscard]] uint32_t nodeHighWaterMark() const noexcept { return next_index_; }

    // -----------------------------------------------------------------------
    //  Node management (render thread only)
    // -----------------------------------------------------------------------

    /**
     * @brief Insert or update the GpuOctreeNode for a given chunk.
     *
     * If @p chunk_id is new, a slot is allocated in the SSBO array.
     * If it already exists, its data is overwritten via staging.
     *
     * @param chunk_id  Stable caller-assigned id (matches PointCloudGlobalBuffer slot)
     * @param node      Node data to write
     * @param ctx       Staging upload parameters (allocator, cmd, staging batch)
     * @return The SSBO array index (can be used in indirect draw setup)
     */
    /// TransferScheduler-aware overload.
    uint32_t upsertNode(uint32_t chunk_id, const GpuOctreeNode& node,
                        TransferScheduler& scheduler);

    /**
     * @brief Write the current node high-water-mark to the SSBO header once.
     *
     * Must be called once per drain sweep after all upsertNode calls for that
     * frame are done.  Writes { node_count = next_index_ } to offset 0 of the
     * buffer via staging so the culling shader knows how many nodes to visit.
     * This avoids the write-after-write hazard of calling it inside upsertNode.
     */
    /// TransferScheduler-aware overload.
    void flushNodeCount(TransferScheduler& scheduler);

    /**
     * @brief Remove the node for a given chunk.
     *
     * The SSBO slot is returned to the freelist and queued for GPU zeroing.
     * Call flushRemovedNodes() once per drain sweep to stage the zero writes.
     */
    void removeNode(uint32_t chunk_id);

    /**
     * @brief Remove all nodes, queuing every slot for GPU zeroing.
     *
     * After this call, chunk_to_index_ is empty and next_index_ is reset to 0.
     * flushRemovedNodes() must still be called to stage the actual zero writes.
     */
    void removeAllNodes();

    /**
     * @brief Stage zero-fill for all slots removed since the last flush.
     *
     * Must be called once per drain sweep (after removeNode calls, before the
     * TRANSFER→COMPUTE barrier) so freed SSBO slots have stream_state=0 and
     * are skipped by the culling shader.
     */
    /// TransferScheduler-aware overload.
    void flushRemovedNodes(TransferScheduler& scheduler);

    /// True if removeNode() was called since the last flushRemovedNodes().
    [[nodiscard]] bool hasPendingRemovals() const noexcept { return !pending_zeros_.empty(); }

    /**
     * @brief Get the SSBO array index for a chunk (for building draw commands).
     * @return Index in [0, nodeCount()), or kInvalidIndex if not found.
     */
    [[nodiscard]] uint32_t getIndex(uint32_t chunk_id) const noexcept;

    static constexpr uint32_t kInvalidIndex = ~uint32_t{0};

private:
    // -----------------------------------------------------------------------
    //  Free slot management
    // -----------------------------------------------------------------------
    uint32_t acquireIndex();
    void     releaseIndex(uint32_t index);

    VmaAllocator  allocator_{nullptr};
    VkBuffer      buffer_   {VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    uint32_t      max_nodes_{0};

    lux::cxx::OffsetSparseSet<uint32_t, uint32_t> chunk_to_index_; ///< chunk_id -> SSBO slot
    std::vector<uint32_t>                         free_indices_;    ///< recycled SSBO slots
    uint32_t                                      next_index_{0};   ///< high-water mark
    std::vector<uint32_t>                         pending_zeros_;   ///< slots awaiting GPU zero-fill

    // Deferred-destroy list for expanded-away buffers.
    DeferredDestroyQueue* deferred_queue_{nullptr};
};

} // namespace lux::render
