#pragma once
/**
 * @file UploadWorkerPool.hpp
 * @brief Async GPU upload via dedicated worker threads + Transfer Queue.
 *
 * Owned by the render thread (RenderServer::Impl).  Workers perform VMA
 * staging allocation, memcpy, and command buffer recording — but DO NOT
 * submit.  Each recorded transfer command buffer is handed to the render
 * thread via an MPSC ring (pending_submits_); the render thread is the SOLE
 * submitter to the Transfer Queue (flushPendingSubmits()), so the queue is
 * touched by exactly one thread.  This removes the cross-thread VkQueue race
 * (a worker's vkQueueSubmit2 colliding with the render thread's frame submit
 * on the same — often family-aliased — VkQueue) WITHOUT any queue-submission
 * mutex.  Completions are drained by the render thread each tick.
 *
 * GPU synchronisation uses a Timeline Semaphore signalled by each render-thread
 * transfer submit; the render thread adds a wait on the graphics queue submit.
 *
 * On integrated GPUs without a dedicated transfer queue the pool falls back
 * to StagingOnly mode: workers still do VMA alloc + memcpy off the render
 * thread, but the render thread itself records vkCmdCopyBuffer on the
 * graphics command buffer.
 */

#include <lux/engine/render/core/RenderTypes.hpp> // EPixelFormat

#include <lux/cxx/concurrent/ThreadPool.hpp>
#include <lux/cxx/concurrent/BlockingQueue.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T *;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T *;

namespace lux::render
{
    class DeviceContext;

    inline constexpr uint32_t kTextureTransferMaxMipCount = 16;

    struct TextureTransferMipRef
    {
        std::shared_ptr<const void> owner{};
        const std::byte *data{nullptr};
        std::size_t bytes{0};
        int32_t width{0};
        int32_t height{0};
    };

    struct TextureTransferMipCopy
    {
        VkDeviceSize buffer_offset{0};
        VkDeviceSize byte_size{0};
        uint32_t mip_level{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    // =========================================================================
    //  Task types  (render-thread handler → upload worker)
    // =========================================================================

    struct MeshTransferTask
    {
        uint32_t mesh_index;

        VkBuffer vbo_buf;
        VkDeviceSize vbo_offset;
        VkDeviceSize vbo_bytes;
        const std::byte *vbo_data;

        VkBuffer ibo_buf;
        VkDeviceSize ibo_offset;
        VkDeviceSize ibo_bytes;
        const std::byte *ibo_data;

        // Pins the source vertex/index memory until the worker thread finishes
        // its async copy. vbo_data/ibo_data point INTO this owner (a shared copy
        // of the client's rdesc::Mesh). Without it the worker read client-owned
        // vectors the documented contract let the caller free right after
        // submitFrame() returns — a use-after-free. Mirrors TextureTransferMipRef.
        std::shared_ptr<const void> data_owner{};

        uint32_t request_id{UINT32_MAX}; ///< Deferred reply: original client RequestId
        uint32_t resource_gen{0};            ///< Handle generation for typed reply
    };

    struct TextureTransferTask
    {
        uint32_t slot_index;
        int32_t width, height, channels;
        EPixelFormat format;
        bool gen_mips;
        uint32_t mip_count{1};
        std::array<TextureTransferMipRef, kTextureTransferMaxMipCount> mips{};

        uint32_t request_id{UINT32_MAX}; ///< Deferred reply: original client RequestId
        uint32_t resource_gen{0};            ///< Handle generation for typed reply
    };

    struct CubeTransferTask
    {
        uint32_t slot_index;
        int32_t face_size, channels;
        EPixelFormat format;
        struct FaceRef
        {
            std::shared_ptr<const void> owner{};
            const std::byte *data;
            std::size_t bytes;
        };
        FaceRef faces[6];

        uint32_t request_id{UINT32_MAX}; ///< Deferred reply: original client RequestId
        uint32_t resource_gen{0};            ///< Handle generation for typed reply
    };
    // =========================================================================

    struct TransferCompletion
    {
        enum class Kind : uint8_t
        {
            MeshBuffer,
            Texture2D,
            TextureCube
        };
        Kind kind{};

        uint64_t timeline_value{};

        // Deferred reply — render thread sends typed resource reply.
        uint32_t request_id{UINT32_MAX};
        uint32_t resource_handle{0}; ///< mesh_index or slot_index
        uint32_t resource_gen{0};    ///< handle generation for typed reply
        VkDeviceSize stg_size{0};

        // Staging buffer — render thread retires after frame fence.
        VkBuffer stg_buf{VK_NULL_HANDLE};
        VmaAllocation stg_alloc{nullptr};

        union
        {
            struct
            {
                VkBuffer vbo_buf;
                VkDeviceSize vbo_offset;
                VkDeviceSize vbo_size;
                VkBuffer ibo_buf; ///< VK_NULL_HANDLE if no IBO
                VkDeviceSize ibo_offset;
                VkDeviceSize ibo_size;
                uint32_t mesh_index;
            } mesh;

            struct
            {
                VkImage image;
                VmaAllocation image_alloc;
                VkImageView view;
                VkSampler sampler;
                VkFormat format;
                uint32_t mip_levels;
                uint32_t array_layers;
                int32_t width, height;
                uint32_t slot_index;
                bool needs_mip_gen;
                uint32_t uploaded_mip_count;
                std::array<TextureTransferMipCopy, kTextureTransferMaxMipCount> uploaded_mips;
            } texture;
        };

        TransferCompletion() { std::memset(this, 0, sizeof(*this)); }
    };

    // =========================================================================
    //  CmdPoolLease
    // =========================================================================

    struct CmdPoolLease
    {
        VkCommandPool pool{VK_NULL_HANDLE};
        uint32_t index{};
    };

    // =========================================================================
    //  PendingSubmit  (upload worker → render thread)
    //
    //  A fully-recorded (but NOT submitted) transfer command buffer plus the
    //  command-pool lease it was recorded into and the completion to publish
    //  once the render thread submits it.  Workers push these; the render
    //  thread drains them in flushPendingSubmits(), assigns the timeline value,
    //  submits to the Transfer Queue, releases the lease, and forwards the
    //  completion (with timeline_value filled in) to the completion ring.
    // =========================================================================
    struct PendingSubmit
    {
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        CmdPoolLease lease{};
        TransferCompletion completion{}; ///< timeline_value filled at submit time
    };

    // =========================================================================
    //  UploadWorkerPool
    // =========================================================================
    class UploadWorkerPool
    {
    public:
        struct Config
        {
            DeviceContext *device_ctx = nullptr;
            uint32_t worker_count = 2;
            uint32_t queue_capacity = 64;
            uint32_t completion_ring = 1024;
            /// worker→render recorded-CB ring capacity. Bounds how many recorded
            /// transfer CBs may sit un-submitted before workers back-pressure
            /// (block on push); the render thread drains it each tick. Also caps
            /// the lease free-list pressure (see cmd_pool_count).
            uint32_t pending_submit_ring = 64;
            uint32_t cmd_pool_count = 0; ///< 0 = pending_submit_ring + worker_count
        };

        explicit UploadWorkerPool(const Config &cfg);
        ~UploadWorkerPool();

        UploadWorkerPool(const UploadWorkerPool &) = delete;
        UploadWorkerPool &operator=(const UploadWorkerPool &) = delete;

        // ── Render-thread handler API ───────────────────────────────────

        void submitMeshTransfer(MeshTransferTask task);
        void submitTextureTransfer(TextureTransferTask task);
        void submitCubeTransfer(CubeTransferTask task);

        /// Generic submit (e.g. point cloud extension).
        template <typename F>
        auto submit(F &&fn) { return pool_.submit(std::forward<F>(fn)); }

        // ── Render-thread tick API ──────────────────────────────────────

        /// Submit up to `max` worker-recorded transfer command buffers to the
        /// Transfer Queue (render thread ONLY — the sole submitter, no lock
        /// needed). Assigns each a timeline value, releases its command-pool
        /// lease, and writes the resulting completion (timeline_value filled in)
        /// into `out`. Returns the count written. Call once per tick before
        /// draining completions; the caller defers these into its pending set
        /// exactly like freshly drained completions. No-op (returns 0) in
        /// StagingOnly mode. Leftover packets beyond `max` stay queued for the
        /// next call.
        uint32_t flushPendingSubmits(TransferCompletion *out, uint32_t max);

        /// Drain completions (non-blocking).  Returns count drained.
        uint32_t drainCompletions(TransferCompletion *out, uint32_t max);

        [[nodiscard]] uint64_t maxCompletedTimelineValue() const noexcept
        {
            return max_completed_timeline_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] VkSemaphore timelineSemaphore() const noexcept { return timeline_sem_; }
        [[nodiscard]] bool hasTransferQueue() const noexcept;
        [[nodiscard]] uint32_t transferFamily() const noexcept { return transfer_family_; }
        [[nodiscard]] uint32_t graphicsFamily() const noexcept { return graphics_family_; }

        void shutdown();

    private:
        enum class UploadMode : uint8_t
        {
            TransferQueue,
            StagingOnly
        };

        CmdPoolLease acquireCmdPool();
        void releaseCmdPool(CmdPoolLease lease);

        // Internal helpers for common worker patterns.
        struct StagingResult
        {
            VkBuffer buf{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void *mapped{nullptr};
        };

        StagingResult allocStagingBuffer(VkDeviceSize bytes);

        /// Destroy the GPU resources held by a completion that will never be
        /// finalized (a packet dropped on a closed pending-submit ring, or left
        /// un-submitted at shutdown). Frees staging + (for textures) the
        /// per-task image/view/sampler so teardown leaks nothing.
        void freeUnsubmittedCompletion(TransferCompletion &c);

        // Convert engine pixel format to VkFormat.
        static VkFormat toVkFormat(EPixelFormat fmt);

        // ── Members ─────────────────────────────────────────────────────

        lux::cxx::ThreadPool pool_;

        // Transfer Queue. Submitted to ONLY by the render thread
        // (flushPendingSubmits) — no queue-submission mutex.
        VkQueue transfer_queue_{VK_NULL_HANDLE};
        uint32_t transfer_family_{};
        uint32_t graphics_family_{};
        bool needs_ownership_transfer_{false};

        // Worker→render recorded-CB hand-off (MPSC). Workers push fully-recorded
        // transfer command buffers here; the render thread drains + submits them.
        lux::cxx::BlockingRingQueue<PendingSubmit> pending_submits_;

        // GPU synchronisation. timeline_counter_ is advanced ONLY by the render
        // thread in flushPendingSubmits(); atomic kept for lock-free reads.
        VkSemaphore timeline_sem_{VK_NULL_HANDLE};
        std::atomic<uint64_t> timeline_counter_{0};
        std::atomic<uint64_t> max_completed_timeline_{0};

        // Completion channel (MPSC). Workers use the BLOCKING push(), never
        // try_push: the render thread drains at most kMaxDrainBatch per tick, so
        // a bulk import can fill the ring. Dropping a completion would leak the
        // staging buffer + worker-created image/view/sampler, leave a mesh's QFOT
        // half-done, and hang the client's deferred reply forever. A full ring
        // means the render thread is behind, so blocking the worker is correct;
        // close() unblocks every waiter at shutdown.
        lux::cxx::BlockingRingQueue<TransferCompletion> completions_;

        // Per-worker command pools
        struct CmdPoolSlot
        {
            VkCommandPool pool{VK_NULL_HANDLE};
            uint64_t last_timeline_value{0};  ///< Timeline value signalled by the last submit using this pool
        };
        std::vector<CmdPoolSlot> cmd_pools_;
        std::vector<uint32_t> free_pool_indices_;
        std::mutex pool_alloc_mutex_;

        UploadMode mode_{UploadMode::TransferQueue};
        VmaAllocator vma_{nullptr};
        VkDevice device_{VK_NULL_HANDLE};
    };

} // namespace lux::render
