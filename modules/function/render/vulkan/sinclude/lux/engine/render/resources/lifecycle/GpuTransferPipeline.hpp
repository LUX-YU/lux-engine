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

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <variant>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    class DeviceContext;

    inline constexpr uint32_t kTextureTransferMaxMipCount = 16;

    struct TextureTransferMipRef
    {
        std::shared_ptr<const void> owner{};
        const std::byte* data{nullptr};
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
        const std::byte* vbo_data;

        VkBuffer ibo_buf;
        VkDeviceSize ibo_offset;
        VkDeviceSize ibo_bytes;
        const std::byte* ibo_data;

        // Pins the source vertex/index memory until the worker thread finishes
        // its async copy. vbo_data/ibo_data point INTO this owner (a shared copy
        // of the client's rdesc::Mesh). Without it the worker read client-owned
        // vectors the documented contract let the caller free right after
        // submitFrame() returns — a use-after-free. Mirrors TextureTransferMipRef.
        std::shared_ptr<const void> data_owner{};

        uint32_t request_id{UINT32_MAX}; ///< Deferred reply: original client RequestId
        uint32_t resource_gen{0};        ///< Handle generation for typed reply
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
        uint32_t resource_gen{0};        ///< Handle generation for typed reply
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
            const std::byte* data{nullptr};
        };
        FaceRef faces[6];

        uint32_t request_id{UINT32_MAX}; ///< Deferred reply: original client RequestId
        uint32_t resource_gen{0};        ///< Handle generation for typed reply
    };
    // =========================================================================

    struct TransferCompletion
    {
        enum class EKind : uint8_t
        {
            MESH_BUFFER,
            TEXTURE_2D,
            TEXTURE_CUBE,
            TEXTURE_2D_REPLACEMENT
        };
        EKind kind{};

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

        union {
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
    using VUploadJob = std::variant<MeshTransferTask, TextureTransferTask, CubeTransferTask>;

    using VGpuTransferResult = std::variant<RecordedBatch, TransferCompletion>;

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
                TransferCompletion::EKind,
                std::uint32_t,
                std::uint32_t,
                EUploadLifecycleState
            ) noexcept;

            DeviceContext* device_ctx = nullptr;
            uint32_t queue_capacity = 64;
            uint32_t result_capacity = 1024;
            uint32_t batch_slot_count = 4;
            NotifyWorkFn notify_work = nullptr;
            void* notify_work_state = nullptr;
            LifecycleFn lifecycle = nullptr;
            void* lifecycle_state = nullptr;
        };

        ~GpuTransferPipeline();

        [[nodiscard]] static Expected<std::unique_ptr<GpuTransferPipeline>> create(const Config& config) noexcept;

        GpuTransferPipeline(const GpuTransferPipeline&) = delete;
        GpuTransferPipeline& operator=(const GpuTransferPipeline&) = delete;
        GpuTransferPipeline(GpuTransferPipeline&&) = delete;
        GpuTransferPipeline& operator=(GpuTransferPipeline&&) = delete;

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
        uint32_t drainResults(TransferCompletion* out, uint32_t max);

        /// Submit a render-thread-recorded graphics finalize batch and arrange
        /// an epoch wake when its timeline value retires.
        [[nodiscard]] std::optional<std::uint64_t> submitGraphicsFinalize(VkCommandBuffer command_buffer);

        /// Release a dedicated-transfer command-pool slot after the matching
        /// graphics queue-family acquire has been successfully queued.
        void releaseAfterGraphicsAcquire(uint32_t batch_slot) noexcept;

        [[nodiscard]] VkSemaphore timelineSemaphore() const noexcept;

        [[nodiscard]] bool needsQueueFamilyOwnershipTransfer() const noexcept;

        [[nodiscard]] uint32_t transferFamily() const noexcept;

        [[nodiscard]] uint32_t graphicsFamily() const noexcept;

        [[nodiscard]] EGpuTransferMode mode() const noexcept;

        [[nodiscard]] std::uint64_t stagingCopiedBytes() const noexcept;

        /// Stop admission, join the worker and retain accepted completions for owner collection.
        /// Native backing remains alive; this is a terminal drain, not reinitialization.
        void stopAndDrain() noexcept;

    private:
        struct State;
        GpuTransferPipeline(std::unique_ptr<State> state, std::thread worker) noexcept;

        std::unique_ptr<State> state_;
        std::thread transfer_thread_;
    };

} // namespace lux::render
