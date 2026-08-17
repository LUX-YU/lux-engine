#pragma once
/**
 * @file GpuTransferPipeline.hpp
 * @brief Single-owner GPU transfer pipeline.
 *
 * Owned by the render thread (RenderServer::Impl). Exactly one transfer thread
 * consumes UploadJob values from one SPSC ring and publishes GpuTransferResult
 * values through the opposite SPSC ring. There is no worker pool, MPSC queue,
 * or command-pool free-list. Vulkan queue entry points use DeviceContext's
 * per-queue external-synchronization gate; the gate protects no engine state.
 *
 * With a distinct transfer VkQueue handle (DEDICATED_QUEUE), the transfer
 * thread owns that queue and performs staging, recording, submission, and
 * timeline completion. When transfer and graphics alias the same handle
 * (RECORD_ONLY), the transfer thread only stages and records; the render thread
 * remains the sole owner that submits the shared graphics queue. A device with
 * no usable transfer command path uses the same record-only result channel and
 * records the copy in the render-thread graphics-finalize control submit.
 */

#include <lux/engine/function/render/client/core/RenderTypes.hpp> // EPixelFormat
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/client/UploadLifecycle.hpp>
#include <lux/engine/function/visibility.h>

#include <lux/cxx/concurrent/LockFreeQueue.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
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
        /// Tight offset of this mip into the packed staging buffer, taken verbatim
        /// from the server's validated Texture2DUploadPlan — the worker consumes it
        /// instead of re-accumulating (single source of truth).
        VkDeviceSize buffer_offset{0};
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
    //  Task types  (render thread → transfer thread)
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
        EPixelFormat format;
        bool gen_mips;
        /// Creation reserves a fresh slot; replacement targets an existing slot
        /// and must never reclaim it on failure.
        bool replacement{false};
        std::uint32_t logical_base_mip{0u};
        uint32_t mip_count{1};
        std::array<TextureTransferMipRef, kTextureTransferMaxMipCount> mips{};
        /// Total packed staging size, from the validated Texture2DUploadPlan
        /// (== sum of per-mip bytes). Worker allocates staging of exactly this
        /// size rather than re-summing.
        VkDeviceSize total_bytes{0};

        uint32_t request_id{UINT32_MAX}; ///< Deferred reply: original client RequestId
        uint32_t resource_gen{0};            ///< Handle generation for typed reply
    };

    struct CubeTransferTask
    {
        uint32_t slot_index;
        int32_t face_size;
        EPixelFormat format;
        /// Authoritative per-face byte size = the staging stride, from the validated
        /// CubeUploadPlan. All six faces are exactly this size, so one value replaces
        /// six equal per-face counts; the worker uses it verbatim as stride/total
        /// instead of recomputing pixelFormatMipBytes().
        VkDeviceSize face_bytes{0};
        struct FaceRef
        {
            std::shared_ptr<const void> owner{};
            const std::byte *data{nullptr};
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
            TextureCube,
            Texture2DReplacement
        };
        Kind kind{};

        /// true → this is a FAILURE terminal state carrying no GPU objects (the
        /// worker already destroyed its own partials). The render thread settles
        /// request_id with status!=0 and reclaims the reserved bindless slot,
        /// instead of leaking the slot and hanging the request.
        bool failed{false};
        /// The transfer thread recorded the GPU copy. False means the result
        /// contains staging data that still needs a graphics control submit.
        bool gpu_copy_recorded{false};
        /// Exclusive resources need a release/acquire pair when the transfer
        /// and graphics queues belong to different families. Long-lived mesh
        /// arenas use concurrent sharing and therefore explicitly clear this.
        bool requires_queue_family_ownership_transfer{true};

        uint64_t timeline_value{};

        // Deferred reply — render thread sends typed resource reply.
        uint32_t request_id{UINT32_MAX};
        uint32_t resource_handle{0}; ///< mesh_index or slot_index
        uint32_t resource_gen{0};    ///< handle generation for typed reply
        uint32_t logical_base_mip{0};
        /// Dedicated-transfer command-pool slot whose release barrier must
        /// remain recorded until the render owner has queued the matching
        /// graphics acquire. UINT32_MAX means that no slot is retained.
        uint32_t retained_batch_slot{UINT32_MAX};
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
                /// Cube only: byte distance between consecutive faces in the staging
                /// buffer = the (validated, format-correct) per-face byte size. The
                /// StagingOnly consumer reads face f at f*face_stride, so it MUST NOT
                /// be re-guessed as width*height*4 at the consumer.
                VkDeviceSize face_stride;
                int32_t width, height;
                uint32_t slot_index;
                bool needs_mip_gen;
                uint32_t uploaded_mip_count;
                std::array<TextureTransferMipCopy, kTextureTransferMaxMipCount> uploaded_mips;
            } texture;
        };

        TransferCompletion()
        {
            std::memset(this, 0, sizeof(*this));
            request_id = UINT32_MAX;
            retained_batch_slot = UINT32_MAX;
            requires_queue_family_ownership_transfer = true;
        }
    };

    // =========================================================================
    //  BatchSlotLease: identifies one fixed transfer-thread batch slot.
    // =========================================================================

    struct BatchSlotLease
    {
        VkCommandPool pool{VK_NULL_HANDLE};
        uint32_t index{};
    };

    // =========================================================================
    //  RecordedBatch  (transfer thread → render thread, RECORD_ONLY only)
    //
    //  A fully-recorded, not-yet-submitted transfer command buffer plus its
    //  fixed batch slot. The render thread submits it on the graphics queue,
    //  assigns a timeline value, and the transfer thread retires the slot after
    //  that value completes.
    // =========================================================================
    struct RecordedBatch
    {
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        BatchSlotLease slot{};
        TransferCompletion completion{}; ///< timeline_value filled at submit time
    };

    // =========================================================================
    using UploadJob = std::variant<
        MeshTransferTask,
        TextureTransferTask,
        CubeTransferTask>;

    using GpuTransferResult = std::variant<RecordedBatch, TransferCompletion>;

    enum class EGpuTransferMode : std::uint8_t
    {
        DEDICATED_QUEUE,
        RECORD_ONLY
    };

    //  GpuTransferPipeline
    // =========================================================================
    class LUX_FUNCTION_PUBLIC GpuTransferPipeline
    {
    public:
        struct Config
        {
            using NotifyWorkFn = void (*)(void*) noexcept;
            using LifecycleFn = void (*)(
                void*,
                std::uint32_t,
                TransferCompletion::Kind,
                std::uint32_t,
                std::uint32_t,
                EUploadLifecycleState
            ) noexcept;

            DeviceContext *device_ctx = nullptr;
            uint32_t queue_capacity = 64;
            uint32_t result_capacity = 1024;
            uint32_t batch_slot_count = 4;
            NotifyWorkFn notify_work = nullptr;
            void* notify_work_state = nullptr;
            LifecycleFn lifecycle = nullptr;
            void* lifecycle_state = nullptr;
        };

        ~GpuTransferPipeline();

        [[nodiscard]] static Expected<
            std::unique_ptr<GpuTransferPipeline>> create(const Config& config);

        GpuTransferPipeline(const GpuTransferPipeline &) = delete;
        GpuTransferPipeline &operator=(const GpuTransferPipeline &) = delete;

        // ── Render-thread handler API ───────────────────────────────────

        [[nodiscard]] bool submitMeshTransfer(MeshTransferTask task);
        [[nodiscard]] bool submitTextureTransfer(TextureTransferTask task);
        [[nodiscard]] bool submitCubeTransfer(CubeTransferTask task);

        // 这里曾有一个 `template<typename F> auto submit(F&&)`,注释写"通用提交
        // (例如点云扩展)"—— 一个假想中的用户。**全仓零外部调用点**(本类内部的
        // pool_.submit 命中全是上面三条定型路径),但它是一个敞着的口子:
        // 结构上没有任何东西拦着有人把渲染资源的活丢上 worker。
        //
        // 删它的理由不是整洁,是**它决定了 J 类那批并发判决的有效期**。那批判决
        // (哪些数据是单线程的、哪把锁可以删、哪个 atomic 可以降级)全部建立在
        // "worker 只碰裸 VkBuffer + 偏移 + 数据持有者"之上。真有人从这个口子把
        // 别的东西丢上去,判决集体失效 —— 而且还会同时撞上 FifOwned 的
        // "retire 只在渲染线程"。要扩展就照着上面三条加一条定型路径,让新用法
        // 显式经过设计,而不是从一个泛型口子溜进来。

        // ── Render-thread tick API ──────────────────────────────────────

        /// Drain the sole transfer→render SPSC. In RECORD_ONLY mode this also
        /// submits recorded command buffers on the render-owned graphics queue.
        uint32_t drainResults(TransferCompletion *out, uint32_t max);

        /// Submit a render-thread-recorded graphics finalize batch and arrange
        /// an epoch wake when its timeline value retires.
        [[nodiscard]] std::optional<std::uint64_t> submitGraphicsFinalize(VkCommandBuffer command_buffer);

        /// Release a dedicated-transfer command-pool slot after the matching
        /// graphics queue-family acquire has been successfully queued.
        void releaseAfterGraphicsAcquire(uint32_t batch_slot) noexcept;

        [[nodiscard]] VkSemaphore timelineSemaphore() const noexcept { return timeline_sem_; }
        [[nodiscard]] bool needsQueueFamilyOwnershipTransfer() const noexcept;
        [[nodiscard]] uint32_t transferFamily() const noexcept { return transfer_family_; }
        [[nodiscard]] uint32_t graphicsFamily() const noexcept { return graphics_family_; }
        [[nodiscard]] EGpuTransferMode mode() const noexcept { return mode_; }
        [[nodiscard]] std::uint64_t stagingCopiedBytes() const noexcept
        {
            return staging_copied_bytes_.load(std::memory_order_relaxed);
        }

        void shutdown();

    private:
        explicit GpuTransferPipeline(const Config& config);
        [[nodiscard]] Expected<void> initialize(const Config& config);

        enum class EBatchSlotState : std::uint8_t
        {
            FREE,
            RECORDING,
            RECORDED,
            SUBMITTED
        };

        [[nodiscard]] BatchSlotLease acquireBatchSlot();
        void releaseBatchSlot(BatchSlotLease slot) noexcept;
        [[nodiscard]] bool retireOneSubmittedSlot();
        [[nodiscard]] bool retireGraphicsFinalize();
        void workerLoop(std::stop_token stop_token);
        void processMeshTransfer(MeshTransferTask task, std::stop_token stop_token);
        void processTextureTransfer(TextureTransferTask task, std::stop_token stop_token);
        void processCubeTransfer(CubeTransferTask task, std::stop_token stop_token);
        [[nodiscard]] bool publishResult(GpuTransferResult result);
        void publishRecorded(RecordedBatch batch);

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

        /// Publish a FAILURE terminal state for a worker task that bailed out
        /// (cancel / invalid data / unknown format / Vulkan/VMA/staging failure).
        /// Carries only the reply identity + reserved slot — NO GPU objects (the
        /// worker destroys its own partials first) — so the render thread settles
        /// the client request with status!=0 and reclaims the reserved bindless
        /// slot, instead of leaking it and hanging the request forever. Meshes
        /// pass slot_index = mesh_index.
        void pushFailure(TransferCompletion::Kind kind, uint32_t request_id,
                         uint32_t slot_index, uint32_t resource_gen,
                         uint32_t logical_base_mip = 0u);
        void notifyLifecycle(
            std::uint32_t request_id,
            TransferCompletion::Kind kind,
            std::uint32_t resource_handle,
            std::uint32_t resource_gen,
            EUploadLifecycleState state
        ) noexcept;

        // Convert engine pixel format to VkFormat; std::nullopt for a format the
        // upload path does not know (never silently substitutes RGBA8).
        static std::optional<VkFormat> toVkFormat(EPixelFormat fmt);

        // ── Members ─────────────────────────────────────────────────────

        lux::cxx::SpscLockFreeRingQueue<UploadJob> jobs_;
        lux::cxx::SpscLockFreeRingQueue<GpuTransferResult> results_;
        std::jthread transfer_thread_;
        std::atomic<bool> accepting_{true};
        std::atomic<bool> worker_running_{false};
        std::atomic<std::uint64_t> job_epoch_{0};
        std::atomic<std::uint64_t> result_space_epoch_{0};
        std::atomic<std::uint64_t> worker_epoch_{0};
        std::atomic<std::uint64_t> graphics_finalize_timeline_{0};
        std::atomic<std::uint64_t> staging_copied_bytes_{0};
        std::vector<TransferCompletion> shutdown_completions_;
        std::size_t shutdown_completion_cursor_{0};

        // Queue ownership is mode-dependent and based on actual handles. The
        // narrow DeviceContext queue gates provide Vulkan's required external
        // synchronization without protecting any engine/business state.
        VkQueue transfer_queue_{VK_NULL_HANDLE};
        VkQueue graphics_queue_{VK_NULL_HANDLE};
        std::mutex* transfer_queue_mutex_{nullptr};
        std::mutex* graphics_queue_mutex_{nullptr};
        uint32_t transfer_family_{};
        uint32_t graphics_family_{};
        bool needs_ownership_transfer_{false};

        // Both queue owners allocate values from the same timeline. The atomic
        // counter is the cross-thread value allocator; every submit additionally
        // waits for N-1 before signalling N because host allocation order alone
        // does not impose execution order between the transfer/graphics queues.
        // Vulkan queue access itself remains single-owner.
        VkSemaphore timeline_sem_{VK_NULL_HANDLE};
        std::atomic<uint64_t> timeline_counter_{0};

        // Fixed batch slots. Only the transfer thread records/resets pools; in
        // RECORD_ONLY mode the render thread changes RECORDED→SUBMITTED after
        // its queue submit. Slot state is the only cross-thread coordination.
        struct CmdPoolSlot
        {
            VkCommandPool pool{VK_NULL_HANDLE};
            std::atomic<EBatchSlotState> state{EBatchSlotState::FREE};
            std::atomic<uint64_t> last_timeline_value{0};
        };
        std::unique_ptr<CmdPoolSlot[]> cmd_pools_;
        uint32_t cmd_pool_count_{0};
        uint32_t next_cmd_pool_{0};

        EGpuTransferMode mode_{EGpuTransferMode::RECORD_ONLY};
        bool can_record_transfer_{false};
        Config::NotifyWorkFn notify_work_{nullptr};
        void* notify_work_state_{nullptr};
        Config::LifecycleFn lifecycle_{nullptr};
        void* lifecycle_state_{nullptr};
        VmaAllocator vma_{nullptr};
        VkDevice device_{VK_NULL_HANDLE};
        DeviceContext* device_context_{nullptr};
        bool shutdown_complete_{false};
    };

} // namespace lux::render
