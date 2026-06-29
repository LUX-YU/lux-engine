#pragma once
/**
 * @file TrajectoryGlobalBuffer.hpp
 * @brief Single large GPU buffer serving as a unified vertex buffer for
 *        all trajectory paths (analogous to PointCloudGlobalBuffer).
 *
 * Memory layout:
 *   [slot_0: first=0,     capacity=C0 ] GpuTrajectoryVertex[C0]
 *   [slot_1: first=C0,    capacity=C1 ] GpuTrajectoryVertex[C1]
 *   ...
 *
 * Allocation strategy: simple freelist over variable-size slots.
 * Thread safety: all methods must be called from the RENDER THREAD only.
 */

#include <lux/engine/render/renderer/features/trajectory/TrajectoryGpuData.hpp>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/render/resources/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/FrameRetireScheduler.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/SparseSet.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace lux::render
{
    class TransferScheduler;

class LUX_FUNCTION_PUBLIC TrajectoryGlobalBuffer
{
public:
    // -----------------------------------------------------------------------
    //  Slot descriptor — describes one trajectory's region in the buffer
    // -----------------------------------------------------------------------
    struct Slot
    {
        uint32_t first_vertex{0};  ///< Start index in the global vertex array
        uint32_t capacity{0};      ///< Allocated capacity (>= vertex_count)
        uint32_t vertex_count{0};  ///< Actual live vertex count (for draw calls)
    };

    TrajectoryGlobalBuffer() = default;
    ~TrajectoryGlobalBuffer() { shutdown(); }

    TrajectoryGlobalBuffer(const TrajectoryGlobalBuffer&)            = delete;
    TrajectoryGlobalBuffer& operator=(const TrajectoryGlobalBuffer&) = delete;
    TrajectoryGlobalBuffer(TrajectoryGlobalBuffer&&)                 = delete;
    TrajectoryGlobalBuffer& operator=(TrajectoryGlobalBuffer&&)      = delete;

    // -----------------------------------------------------------------------
    //  Lifecycle
    // -----------------------------------------------------------------------

    bool init(VmaAllocator allocator, uint32_t max_vertices);
    void shutdown();

    void setDeferredQueue(DeferredDestroyQueue* q) noexcept { deferred_queue_ = q; }
    void setRetireScheduler(FrameRetireScheduler* rs) noexcept { retire_scheduler_ = rs; }
    void setRetireOwnerToken(FrameRetireScheduler::OwnerToken token) noexcept { retire_owner_token_ = token; }

    [[nodiscard]] bool isInitialized() const noexcept { return buffer_ != VK_NULL_HANDLE; }

    // -----------------------------------------------------------------------
    //  Slot management (render thread only)
    // -----------------------------------------------------------------------

    /**
     * @brief Reserve a slot for @p capacity vertices under a caller-supplied ID.
     * @return true on success; false if buffer is full or trajectory_id already exists.
     */
    bool reserveSlot(uint32_t trajectory_id, uint32_t capacity);

    /**
     * @brief Ensure @p trajectory_id has a slot with at least @p capacity vertices.
     * May trigger buffer growth.
     */
    /// TransferScheduler-aware overload.
    bool ensureSlotCapacity(uint32_t trajectory_id,
                            uint32_t capacity,
                            TransferScheduler& scheduler);

    /**
     * @brief Schedule deferred release of a slot.
     */
    void freeSlot(uint32_t trajectory_id);

    /**
     * @brief Upload (overwrite) vertex data into an existing slot via staging.
     * Resets vertex_count to data.size().
     */
    /// TransferScheduler-aware overload.
    bool upload(uint32_t trajectory_id,
                std::span<const GpuTrajectoryVertex> data,
                TransferScheduler& scheduler);

    /// TransferScheduler-aware overload.
    uint32_t append(uint32_t trajectory_id,
                    std::span<const GpuTrajectoryVertex> data,
                    TransferScheduler& scheduler);

    /**
     * @brief Reset a trajectory's vertex count to 0 (no GPU work needed).
     */
    void clearTrajectory(uint32_t trajectory_id) noexcept;

    /**
     * @brief Update vertex count without uploading data.
     */
    void setVertexCount(uint32_t trajectory_id, uint32_t count) noexcept;

    // -----------------------------------------------------------------------
    //  Queries
    // -----------------------------------------------------------------------

    [[nodiscard]] std::optional<Slot> getSlot(uint32_t trajectory_id) const noexcept;
    [[nodiscard]] VkBuffer buffer() const noexcept { return buffer_; }
    [[nodiscard]] uint32_t maxVertices() const noexcept { return max_vertices_; }
    [[nodiscard]] uint32_t usedVertices() const noexcept;

    [[nodiscard]] bool hasSlot(uint32_t trajectory_id) const noexcept
    {
        return slots_.contains(trajectory_id);
    }

    /**
     * @brief Iterate over all active slots.
     * Callback: void fn(uint32_t trajectory_id, const Slot& slot)
     */
    template<typename Fn>
    void forEachTrajectory(Fn&& fn) const
    {
        const auto& keys = slots_.keys();
        const auto& values = slots_.values();
        const std::size_t count = std::min(keys.size(), values.size());
        for (std::size_t i = 0; i < count; ++i)
            fn(keys[i], values[i]);
    }

    static constexpr uint32_t kInvalidTrajectoryId = ~uint32_t{0};

    // -----------------------------------------------------------------------
    //  Frame lifecycle
    // -----------------------------------------------------------------------

private:
    struct FreeRegion
    {
        uint32_t first;
        uint32_t capacity;
    };

    uint32_t tryAllocFromFreeList(uint32_t capacity);
    void     returnToFreeList(uint32_t first, uint32_t capacity);
    uint32_t allocOrGrow(uint32_t capacity, TransferScheduler& scheduler);
    bool     growBuffer(uint32_t required_capacity, TransferScheduler& scheduler);

    VmaAllocator  allocator_{nullptr};
    VkBuffer      buffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    uint32_t      max_vertices_{0};

    lux::cxx::OffsetSparseSet<uint32_t, Slot> slots_;
    std::vector<FreeRegion>               free_list_;
    DeferredDestroyQueue*                 deferred_queue_{nullptr};
    FrameRetireScheduler*                 retire_scheduler_{nullptr};
    FrameRetireScheduler::OwnerToken      retire_owner_token_{0};
};

} // namespace lux::render
