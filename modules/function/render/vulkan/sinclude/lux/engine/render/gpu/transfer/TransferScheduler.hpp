#pragma once
/**
 * @file TransferScheduler.hpp
 * @brief Unified CPU→GPU transfer subsystem with minimal barriers.
 *
 * TransferScheduler collects buffer/image copy requests from resource
 * classes, then emits them with at most two vkCmdPipelineBarrier2 calls:
 *   1. Pre-copy barriers  (reader stages → TRANSFER_WRITE)
 *   2. Post-copy barriers (TRANSFER_WRITE → reader stages)
 *
 * Staging memory is managed internally via a ring buffer with overflow
 * fallback to dedicated VMA allocations.
 *
 * Ownership: one instance per scene + one global instance.
 * Thread safety: render-thread only.
 */

#include <lux/engine/render/gpu/transfer/TransferTypes.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>
#include <lux/engine/render/gpu/utils/StagingRingBuffer.hpp>
#include <lux/engine/render/gpu/memory/StagingBuffer.hpp>
#include <lux/engine/function/visibility.h>

#include <span>
#include <vector>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T *;

namespace lux::render
{

class LUX_FUNCTION_PUBLIC TransferScheduler
{
public:
    struct Config
    {
        VmaAllocator  allocator{nullptr};
        VkDeviceSize  ring_capacity{4 * 1024 * 1024}; ///< Ring buffer size (default 4 MiB)
        uint32_t      frames_in_flight{2};
    };

    TransferScheduler() = default;
    ~TransferScheduler();

    TransferScheduler(const TransferScheduler &) = delete;
    TransferScheduler &operator=(const TransferScheduler &) = delete;
    TransferScheduler(TransferScheduler &&) noexcept;
    TransferScheduler &operator=(TransferScheduler &&) noexcept;

    /// Initialize the scheduler. Must be called before any other method.
    [[nodiscard]] bool init(const Config &cfg);

    /// Tear down internal resources.
    void shutdown();

    // =====================================================================
    //  Staging allocation (called by resource classes)
    // =====================================================================

    /**
     * @brief Allocate staging memory for a CPU→GPU copy.
     *
     * Tries the ring buffer first; falls back to a dedicated VMA buffer
     * on overflow.  The scheduler manages lifetime — callers must not
     * free the returned allocation.
     *
     * @param bytes  Required staging size in bytes.
     * @return StagingAlloc with mapped pointer, or empty on failure.
     */
    [[nodiscard]] StagingAlloc allocateStaging(VkDeviceSize bytes);

    // =====================================================================
    //  Copy request submission (called by resource classes)
    // =====================================================================

    void submitBufferCopy(const BufferCopyRequest &req);
    void submitImageCopy(const ImageCopyRequest &req);
    void submitQFOTAcquire(const QFOTAcquireRequest &req);

    /// Submit a pre-built buffer barrier (e.g. HOST_WRITE→SHADER_READ from
    /// DynamicSSBO) directly into the post-barrier batch.
    void submitExtraPostBarrier(const VkBufferMemoryBarrier2 &barrier);

    // =====================================================================
    //  Contributor management
    // =====================================================================

    TransferContributorList &contributors() noexcept { return contributors_; }

    // =====================================================================
    //  Frame execution (called by Renderer / RenderScene)
    // =====================================================================

    /**
     * @brief Collect transfers from all contributors + emit barriers + copies.
     *
     * Equivalent to: collectFromContributors() → beginTransfers(cmd) →
     *                recordCopies(cmd) → endTransfers(cmd).
     */
    void executeAll(VkCommandBuffer cmd);

    /// Invoke all registered contributors to submit their copy requests.
    void collectFromContributors();

    /// Phase 1: emit merged pre-copy barriers.
    void beginTransfers(VkCommandBuffer cmd);

    /// Phase 2: record all buffer/image copies to cmd.
    void recordCopies(VkCommandBuffer cmd);

    /// Phase 3: emit merged post-copy barriers + QFOT acquires.
    void endTransfers(VkCommandBuffer cmd);

    /// Reset per-frame state (copy lists, ring pointer). Call at frame start.
    /// @param frame_slot  Current FIF slot index (0 .. frames_in_flight-1).
    void resetFrame(uint32_t frame_slot);

    /// Retire overflow staging buffers for completed frame slot.
    /// Call after the frame fence for @p frame_slot has been waited on.
    void retireStaging(uint32_t frame_slot);

    // =====================================================================
    //  Statistics
    // =====================================================================

    [[nodiscard]] uint32_t lastBarrierCount() const noexcept { return last_barrier_count_; }
    [[nodiscard]] uint32_t lastCopyCount() const noexcept { return last_copy_count_; }
    [[nodiscard]] bool hasWork() const noexcept;
    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }

private:
    void buildPreBarriers();
    void buildPostBarriers();

    // ── Members ──────────────────────────────────────────────────────────

    VmaAllocator allocator_{nullptr};
    uint32_t     frames_in_flight_{2};

    // Staging memory
    StagingRingBuffer              ring_;
    std::vector<StagingBuffer>     overflow_staging_;       ///< Current frame overflow
    std::vector<std::vector<StagingBuffer>> deferred_staging_; ///< Per-FIF ring for retirement

    // Transfer contributors
    TransferContributorList contributors_;

    // Per-frame request lists
    std::vector<BufferCopyRequest>  buffer_copies_;
    std::vector<ImageCopyRequest>   image_copies_;
    std::vector<QFOTAcquireRequest> qfot_acquires_;
    std::vector<VkBufferMemoryBarrier2> extra_post_barriers_; ///< Host-write barriers etc.

    // Pre-built barrier arrays (reused across frames to avoid reallocation)
    std::vector<VkBufferMemoryBarrier2> pre_buffer_barriers_;
    std::vector<VkBufferMemoryBarrier2> post_buffer_barriers_;
    std::vector<VkImageMemoryBarrier2>  pre_image_barriers_;
    std::vector<VkImageMemoryBarrier2>  post_image_barriers_;

    // Statistics
    uint32_t last_barrier_count_{0};
    uint32_t last_copy_count_{0};

    bool initialized_{false};
};

} // namespace lux::render
