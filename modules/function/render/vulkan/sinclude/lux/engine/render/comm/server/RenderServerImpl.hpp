#pragma once

// ─────────────────────────────────────────────────────────────────────────
//  GeneralRenderServer::Impl — full definition (sinclude-visible)
//
//  Moved here from RenderServer.cpp so that UI-module subclasses
//  (UIRenderServer) can directly access Impl members.
// ─────────────────────────────────────────────────────────────────────────

#include <lux/engine/render/comm/server/RenderServer.hpp>        // GeneralRenderServer, FrameReplyBuilder
#include <lux/engine/function/render/client/RenderProtocol.hpp>  // FeatureFactory, TypeId
#include <lux/engine/render/gpu/VulkanContext.hpp>               // InstanceContext, DeviceContext, ResourceContext
#include <lux/engine/render/gpu/RenderContext.hpp>               // RenderContext
#include <lux/engine/function/render/client/core/FrameStamp.hpp> // FrameClock, FrameStamp
#include <lux/engine/render/core/RenderErrorSink.hpp>            // 自发上报汇集器(渲染线程私有)
#include <lux/engine/render/renderer/FrameOrchestrator.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>          // Renderer
#include <lux/engine/render/gpu/RenderSurface.hpp>          // RenderSurface
#include <lux/engine/render/targets/SwapchainProvider.hpp>  // SwapchainProvider
#include <lux/engine/render/targets/PresentContext.hpp>     // PresentContext(拆层)
#include <lux/engine/render/targets/OffscreenImagePool.hpp> // OffscreenImagePool
#include <lux/engine/render/renderer/FrameDriver.hpp>       // FrameDriver
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>
#include <lux/engine/render/gpu/memory/StagingBuffer.hpp>         // StagingBuffer
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>    // kMaxFramesInFlight
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // kMaxFramesInFlight

#include <lux/cxx/concurrent/LockFreeQueue.hpp> // SpscLockFreeRingQueue(校验消息通道)
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>                // SlotKeyAutoSparseSet
#include <lux/engine/render/renderer/RenderTargetRegistry.hpp> // targets 一等对象(L4)
#include <lux/cxx/container/SmallVector.hpp>                   // RenderTargetEntry::layers

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::render
{
    class OffscreenImagePool;
    // FeatureTypeRecord + the feature-type registry moved down to the
    // Renderer-owned FeatureTypeRegistry (renderer/FeatureTypeRegistry.hpp), so
    // dependency resolution can reach factories. The comm layer registers types
    // INTO it (renderer_->featureTypeRegistry()) and still binds their ops here.

    //(handle_cast 已归位到 L0 的 core/RenderResourceHandle.hpp —— 它转换的
    // 两种句柄都是 L0 类型,而住在这里会逼得每个只想转句柄的装配 TU
    // include 整个服务端 Impl 头。)

    // ── 校验层消息的跨线程通道 ────────────────────────────────────────────
    //
    // Vulkan 校验层的回调**在哪个线程发起调用就在哪个线程回调**,而上传走
    // `GpuTransferPipeline` 的单一 transfer 线程(专用队列模式下由它
    // vkQueueSubmit2)。于是校验消息的生产者是「渲染线程 + transfer 线程」,
    // 而 `RenderErrorSink` 是个
    // 普通的非原子结构(线性扫描找同键 + 就地自增)—— 两个线程同时 emit 就是
    // 数据竞争。
    //
    // sink 的合并语义要求单写者,而校验回调天然是多线程的。这两件事不塞进同一个
    // 结构:**环负责跨线程传输,sink 继续负责合并聚合**,渲染线程在
    // `flushErrorEvents()` 里把环排空、折进 sink。sink 从此只被渲染线程碰。
    //
    // 放在这里而不是单独一个头:唯一的使用者就是下面的 Impl。搁进
    // RenderErrorSink.hpp 会把 LockFreeQueue 拖进每个 include RenderContext 的
    // TU —— 那个头被五处广用头包着。

    /// 一条校验层消息压缩后的形态。全文过不了 comm(变长),这里只带走能合并
    /// 计数的两维。
    struct ValidationEvent
    {
        std::uint32_t severity{0};    ///< 0=info 1=warn 2=error
        std::uint32_t fingerprint{0}; ///< 消息全文的 FNV-1a,同消息同指纹
    };

    /// 每生产者线程一条 SPSC 环。
    ///
    /// 生产者数量有界且已知,给每个线程发一条自己的环之后,每条就退化成严格
    /// SPSC —— 唯一的写者是拥有它的线程,唯一的读者是渲染线程,正好是
    /// `SpscLockFreeRingQueue` 的契约。一条 MPSC 环要在游标上做 CAS 重试,
    /// 在校验消息暴发时(一帧几百条)反而是更差的形状。
    ///
    /// 线程槽位在该线程**第一次 emit** 时领取,之后固定不变。槽位号是进程级的,
    /// 所以多个实例(多服务器的测试场景)对同一线程给出同一个槽位 —— 各实例的
    /// lane 数组互不相干,同一线程在任何实例上都只写自己那条,SPSC 不变式成立。
    ///
    /// 槽位耗尽(线程数超过 kLanes)与环写满都不阻塞、不分配:计入 dropped_。
    /// 丢的是**最新**的 —— 与 RenderErrorSink 同策略,第一条的诊断价值最高。
    class ValidationEventRing
    {
    public:
        /// 生产者线程上限。渲染线程 + 上传工作线程,8 条留足余量。
        ///
        /// 每条环用 `SpscLockFreeRingQueue` 的默认深度(64,其中一个槽用于区分
        /// 空/满)。合计 500 条上下的缓冲 —— 一个 tick 内积压到这个量级说明校验层
        /// 已经在刷屏,再多留也不增加诊断价值。深度没有做成可配:`std::array`
        /// 传不了逐元素构造参数,而队列不可移动,为改深度换 `unique_ptr` 数组
        /// 等于给每次 emit 加一次堆间接。
        static constexpr std::size_t kLanes = 8;

        /// 记一条。**任意线程**可调,无锁无分配(环的缓冲在构造时一次分配完)。
        void emit(std::uint32_t severity, std::uint32_t fingerprint) noexcept
        {
            const std::size_t lane = laneIndex();
            if (lane >= kLanes ||
                lanes_[lane].tryPush(ValidationEvent{severity, fingerprint}) != lux::cxx::EQueuePushResult::ACCEPTED)
                dropped_.fetch_add(1, std::memory_order_relaxed);
        }

        /// 排空所有通道,对每条事件调用 @p fn。**只能在消费者线程(渲染线程)调**。
        ///
        /// 逐通道排空而不是按时间归并:跨通道的先后本来就没有全序可言(不同线程上
        /// 发生的两件事之间没有同步点),硬排一个顺序是假精度。合并计数在 sink 里
        /// 按键做,与到达顺序无关。
        template <class F> void drain(F&& fn)
        {
            ValidationEvent event{};
            for (auto& lane : lanes_)
                while (lane.tryPop(event) == lux::cxx::EQueuePopResult::VALUE)
                    fn(event);
        }

        /// 因槽位耗尽或环满而丢弃的条数。
        [[nodiscard]] std::uint32_t dropped() const noexcept
        {
            return dropped_.load(std::memory_order_relaxed);
        }

        void clearDropped() noexcept
        {
            dropped_.store(0, std::memory_order_relaxed);
        }

    private:
        /// 本线程的通道号,首次调用时领取并固定。计数器与 thread_local 都是进程级:
        /// 需要的性质只是「同一线程在同一实例上始终写同一条,且没有第二个线程写
        /// 它」,进程级映射满足,而且省掉了按实例注册线程的账。
        static std::size_t laneIndex() noexcept
        {
            static std::atomic<std::size_t> s_next_lane{0};
            static constexpr std::size_t kUnassigned = static_cast<std::size_t>(-1);

            thread_local std::size_t t_lane = kUnassigned;
            if (t_lane == kUnassigned)
                t_lane = s_next_lane.fetch_add(1, std::memory_order_relaxed);
            return t_lane;
        }

        /// 注意是默认初始化而非 `{}`:队列的默认构造函数是 explicit 的
        /// (`explicit SpscLockFreeRingQueue(std::size_t capacity = 64)`),
        /// 值初始化会走每元素的 copy-list-init 而编不过。
        std::array<lux::cxx::SpscLockFreeRingQueue<ValidationEvent>, kLanes> lanes_;
        std::atomic<std::uint32_t> dropped_{0};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  GeneralRenderServer::Impl — owns the full Vulkan stack
    // ─────────────────────────────────────────────────────────────────────
    struct GeneralRenderServer::Impl
    {
        // Back-pointer to the owning server — set in constructor.
        // Allows anonymous-namespace handlers to call server public methods.
        GeneralRenderServer* server_{nullptr};

        // Vulkan infrastructure — created lazily by init()
        std::unique_ptr<InstanceContext> inst_ctx_;
        std::unique_ptr<DeviceContext> dev_ctx_;
        std::unique_ptr<ResourceContext> res_ctx_;
        std::shared_ptr<RenderContext> render_ctx_;
        std::unique_ptr<Renderer> renderer_;

        // Dispatch
        Dispatcher dispatcher;
        // Feature-type registry now lives in renderer_->featureTypeRegistry().

        // Surface + targets
        // (surface_/swapchain_provider_ 单例成员已消亡:呈现机件收进
        //  Surface RenderTargetEntry::present——多窗即多实例。拆层。)
        std::unique_ptr<FrameDriver> frame_driver_;

        // ── RenderTarget 一等对象(设计 §1;收编四份并列状态)──────────
        // Offscreen 态拥有图像池;Surface 态引用 target 自己拥有的
        // swapchain provider(支持多窗)。层链先承载 SceneViewLayer
        //(UILayer 随 imgui 收编启用)。
        // RenderTargetEntry / TargetSet / 全部 target 查询与池管理已搬到 L4 的
        // RenderTargetRegistry —— 那些词汇(渲染目标、合成层、图像池、呈现上下文)
        // 全是渲染层的,没有一个属于线协议;它们住在这里还直接卡住了帧编排的下沉。
        // 本层保留一个持有者与若干转发,调用点照旧。
        RenderTargetRegistry targets_registry_;

        using RenderTargetEntry = RenderTargetRegistry::Entry;

        [[nodiscard]] RenderTargetRegistry& targets() noexcept
        {
            return targets_registry_;
        }
        [[nodiscard]] const RenderTargetRegistry& targets() const noexcept
        {
            return targets_registry_;
        }

        // ── 兼容转发(逐步收敛到直接用 targets()) ────────────────────────
        RenderTargetEntry* surfaceTarget() noexcept
        {
            return targets_registry_.surfaceTarget();
        }
        PresentContext* surfacePresent() noexcept
        {
            return targets_registry_.surfacePresent();
        }
        SwapchainProvider* swapchainProvider() noexcept
        {
            return targets_registry_.swapchainProvider();
        }

        RenderTargetEntry* findOffscreenByView(RenderSceneId s, ViewHandle v) noexcept
        {
            return targets_registry_.findOffscreenByView(s, v);
        }

        bool detachLayerAndReapIfEmpty(RenderTargetId key, RenderSceneId s, ViewHandle v, uint64_t retire_serial)
        {
            return targets_registry_.detachLayerAndReapIfEmpty(key, s, v, retire_serial);
        }

        std::unique_ptr<OffscreenImagePool>
        makeTargetPool(const RenderTargetLayout& layout, VkExtent2D extent, uint32_t target_flags)
        {
            return targets_registry_.makeTargetPool(layout, extent, target_flags);
        }

        void retireTargetPool(RenderTargetEntry& t, uint64_t retire_serial)
        {
            targets_registry_.retireTargetPool(t, retire_serial);
        }

        /// surface + swapchain + Surface target entry 一体创建(attach
        /// 宿主封装与 CreateSurfaceTarget 命令共用)。过渡期所有权在 Impl
        /// 单实例;失败时接管并销毁传入的 surface。
        Expected<RenderTargetId> createSurfaceTargetInternal(RenderSurface&& surface, VkExtent2D extent);

        /// 两阶段销毁的在途账本:Surface target 受理销毁后停止呈现,
        /// 等 fence 水位越过 retire_serial 再拆 swapchain/surface,然后按
        /// request_id 延迟回执 TargetReleased(宿主此后才可销毁平台窗口)。
        struct PendingSurfaceRelease
        {
            RenderTargetId target{};
            uint64_t retire_serial{0};
            /// 0 = 内部释放(imgui 副视口等,不发 TargetReleased 回执);
            /// 非 0 = 命令面销毁,按此 request_id 延迟回执。
            uint32_t request_id{0};
            bool torn_down{false}; ///< 已拆资源,回执待送(环满重试)
            /// 受理销毁时从 entry 移入的呈现机件(per-target 所有权);
            /// fence 水位越过后 reset() 即完成 swapchain→surface 逆序拆。
            std::unique_ptr<PresentContext> ctx;
            /// 随拆除一并执行的收尾(副视口顶点环 / ViewportData 释放
            /// ——同样要等 fence 水位)。
            std::function<void()> on_teardown;
        };
        std::vector<PendingSurfaceRelease> pending_surface_releases_;
        RenderTargetId findOffscreenKeyByView(RenderSceneId s, ViewHandle v) const noexcept
        {
            return targets_registry_.findOffscreenKeyByView(s, v);
        }

        RenderSceneId current_bulk_scene_{};

        // Frame timing — single authoritative clock
        FrameOrchestrator frame_orchestrator_{2};
        FrameStamp current_stamp_{};

        // Per-tick scene/view batch, reused across ticks (cleared at tick start)
        // instead of stack-constructing 3 vectors every frame. MUST be cleared
        // before the offscreen-view loop — stale scene/view pointers would feed
        // endViewFrame after a scene/view was destroyed. (P-5)
        SceneViewBatch scene_view_batch_;
        uint32_t frames_in_flight_{2};
        bool enable_vsync_{true};

        // Single-owner GPU transfer pipeline (built by init; handlers publish
        // low-level jobs, this render owner drains results).
        std::unique_ptr<GpuTransferPipeline> transfer_pipeline_;

        // Deferred staging-buffer deletion list, tagged with the serial current
        // when the buffer was retired (an upper bound of its last GPU use).
        // Freed in beginTickFrame() once the FENCE-PROVEN completion watermark
        // (FrameDriver::gpuCompletedSerial) passes the tag. Formerly a per-FIF
        // slot ring cleared on slot reuse — unsound: serials advance on ticks
        // that never submit (the no-views early return right after a
        // DestroyScene), so slot reuse did not imply the tagged frame finished
        // (caught by scene_cycle_stress_test).
        std::vector<std::pair<uint64_t, StagingBuffer>> async_deferred_staging_;

        // Deferred offscreen-pool deletion list (same tagged-serial contract as
        // the staging list above). DestroyScene moves a scene's
        // OffscreenImagePools here (whole-pool GPU resources) instead of
        // freeing them immediately — keeps the win (no full-device
        // vkDeviceWaitIdle per scene teardown) with sound completion gating.
        // (延迟池列表已随 target 一并搬进 RenderTargetRegistry —— 它记的是
        //  target 池的退休,归目标注册表管;老化调用见 beginTickFrame。)

        // Staging buffers finalized during the drain phase.
        // Moved into async_deferred_staging_ at endTickFrame(), AFTER
        // runUploadPhase() has recorded all copies that read them.
        // On ticks that skip the upload phase (minimised window / failed
        // rebuild) they stay here: StagingOnly copy records remain queued
        // until a future runUploadPhase, so the FIF retirement countdown
        // must not start before the copies are actually recorded.
        std::vector<StagingBuffer> staging_pending_this_tick_;

        // Scratch buffer for drainCompletions() — avoids per-tick allocation.
        static constexpr uint32_t kMaxDrainBatch = 64;
        TransferCompletion completion_buf_[kMaxDrainBatch];

        // VRAM budget: set when DEVICE_LOCAL usage > 95%, cleared when < 85%.
        bool expansion_suppressed_{false};

        // Deferred resource-upload replies: accumulated during drainCompletions,
        // flushed into the reply builder on the next drainAndDispatch cycle.
        struct DeferredReplyEntry
        {
            uint32_t request_id;
            TransferCompletion::Kind kind;
            uint32_t resource_index; ///< handle.index
            uint32_t resource_gen;   ///< handle.gen
            uint32_t logical_base_mip{0};
            uint32_t status{0}; ///< 0 = success; non-zero = failed upload
        };
        std::vector<DeferredReplyEntry> pending_deferred_replies_;

        struct ActiveUpload
        {
            std::uint32_t request_id{UINT32_MAX};
            TransferCompletion::Kind kind{};
            std::uint32_t resource_index{0};
            std::uint32_t resource_gen{0};
            EUploadLifecycleState state{EUploadLifecycleState::Accepted};
        };
        std::vector<ActiveUpload> active_uploads_;
        UploadLifecycleSnapshot upload_lifecycle_{};
        static constexpr std::size_t kRecentUploadTerminals = 256;
        std::array<std::uint32_t, kRecentUploadTerminals> recent_upload_terminals_{};
        std::size_t recent_upload_terminal_cursor_{0};

        struct PendingGraphicsFinalize
        {
            std::uint64_t timeline_value{0};
            VkCommandBuffer command_buffer{VK_NULL_HANDLE};
            std::vector<DeferredReplyEntry> replies;
            std::vector<StagingBuffer> staging;
        };
        std::vector<PendingGraphicsFinalize> pending_graphics_finalizes_;
        std::vector<DeferredReplyEntry> graphics_finalize_reply_batch_;
        std::vector<StagingBuffer> graphics_finalize_staging_batch_;
        std::vector<std::uint32_t> graphics_finalize_slot_batch_;
        bool graphics_finalize_required_{false};

        /// 本 tick 汇集到的自发上报(没有请求可回的那些失败)。定长、无分配;
        /// 由 flushErrorEvents() 每 tick 打包推出并清空。
        ///
        /// **只被渲染线程碰。** 它按键做线性合并,不是线程安全结构。来自其它线程的
        /// 校验层消息先进 validation_ring_,由 flushErrorEvents() 折进来。
        RenderErrorSink error_sink_;

        /// 校验层消息的跨线程入口。校验回调在发起 Vulkan 调用的那个线程上跑,
        /// 而上传走单一 transfer 线程 —— 生产者不止渲染线程一个。
        /// 每生产者一条 SPSC 环,渲染线程在 flushErrorEvents() 里排空。
        ValidationEventRing validation_ring_;

        // Completions whose GPU timeline value hasn't been reached yet.
        // Re-checked each tick via non-blocking vkGetSemaphoreCounterValue.
        std::vector<TransferCompletion> pending_completions_;

        // In-flight async readbacks (ReadbackTargetAsync). Each entry settles
        // `settle_left` ticks, submits a one-shot image->buffer copy, then is
        // polled each tick; once the fence signals the pixels are copied into
        // dst_ptr and a deferred reply (by request_id) is sent. Lives here so it
        // survives across ticks (unlike the synchronous handleReadbackTarget).
        struct PendingReadback
        {
            RenderTargetId target{};
            uint64_t dst_ptr{0};
            uint64_t dst_capacity{0};
            TargetSlot slot{TargetSlot::SCENE_COLOR}; ///< which output semantic to read
            uint32_t request_id{0};
            uint32_t settle_left{0}; ///< ticks to render before the copy
            uint32_t deadline{0};    ///< ticks to wait for the fence
            bool submitted{false};
            bool done{false}; ///< reply filled, ready to send
            // GPU state — valid only between submit and completion.
            VkBuffer buf{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void* mapped{nullptr};
            VkFence fence{VK_NULL_HANDLE};
            VkCommandBuffer cb{VK_NULL_HANDLE};
            uint32_t width{0};
            uint32_t height{0};
            uint32_t bpp{0};
            VkFormat format{VK_FORMAT_UNDEFINED};
            uint64_t needed{0};
            ReadbackTargetReply reply{};
        };
        std::vector<PendingReadback> pending_readbacks_;

        /// Callback invoked before a scene is destroyed — lets subclasses
        /// clean up per-scene cached pointers (e.g. UI offscreen views).
        ///(类型本身已提到命名空间层 —— 它出现在 GeneralRenderServer 的
        /// protected 扩展面签名里,而那个头不认识 Impl。)
        lux::render::PreDestroySceneCallback pre_destroy_scene_cb_{nullptr};

        /// Opaque pointer for subclass-specific data (e.g. UIRenderServer).
        /// Impl does not own or destroy this — the subclass manages its lifetime.
        void* extension_{nullptr};

        Impl()
        {
            recent_upload_terminals_.fill(UINT32_MAX);
        }
        ~Impl();

        /// Two-phase init: creates the full Vulkan stack.
        Expected<void> init(ServerConfig cfg);

        /// Begin a tick frame: retire slot-local staging, advance renderer frame state,
        /// and update VRAM budget flags.
        /// Must be called after FrameDriver::beginFrame() has waited the slot fence.
        LUX_FUNCTION_PUBLIC void beginTickFrame();

        /// End a tick frame: advance frame counter, call renderer endFrame.
        /// Call at the end of tick() after all render calls.
        /// `uploads_recorded` = whether runUploadPhase() ran this tick; when
        /// false, drained staging buffers stay pending instead of entering
        /// the FIF retirement ring (their copies are not recorded yet).
        LUX_FUNCTION_PUBLIC void endTickFrame(bool uploads_recorded);

        // ── Completion processing (called between acquireAndExecute / finalizeReplies) ──

        /// Flush deferred replies + drain/process upload completions (non-blocking).
        [[nodiscard]] bool processUploadCompletions();
        void transitionUpload(
            std::uint32_t request_id,
            TransferCompletion::Kind kind,
            std::uint32_t resource_index,
            std::uint32_t resource_gen,
            EUploadLifecycleState state
        ) noexcept;
        [[nodiscard]] bool observeTransferResult(const TransferCompletion& completion) noexcept;
        void settleUploadReply(const DeferredReplyEntry& reply) noexcept;
        [[nodiscard]] UploadLifecycleSnapshot uploadLifecycle() const noexcept;
        void failActiveUploadsForShutdown() noexcept;

    private:
        /// What a per-kind finalizer did with a completion, so finalizeCompletion knows
        /// whether to send a SUCCESS reply + retire the staging buffer, or to treat it
        /// as failed. A finalizer that drops a completion (stale/recycled slot) frees the
        /// GPU objects AND the staging itself, so the caller must NOT retire the staging
        /// again (double-free) nor claim success.
        enum class FinalizeDisposition
        {
            Succeeded,
            Failed
        };

        // Per-kind completion finalization.
        FinalizeDisposition
        finalizeMeshCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        FinalizeDisposition
        finalizeTexture2DCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        FinalizeDisposition
        finalizeTextureCubeCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        /// Finalize one completion + accumulate deferred reply + defer staging buffer.
        void finalizeCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        /// Return a reserved-but-failed bindless slot to its free list.
        void reclaimReservedSlot(const TransferCompletion& c);
        /// Destroy the worker-created GPU objects (image/view/sampler + staging) of a
        /// texture/cube completion we will NOT finalize — dead/recycled slot or device
        /// loss — so they are not leaked.
        void freeCompletionTextureGpu(const TransferCompletion& c);
        /// Destroy ALL GPU objects a not-yet-finalized completion holds, dispatched by
        /// kind (mesh: staging only; texture/cube: image/view/sampler + staging) so the
        /// union is never mis-read. Used at teardown and on device loss.
        void destroyUnfinalizedCompletion(const TransferCompletion& c);
        [[nodiscard]] bool pollGraphicsFinalizes(std::uint64_t gpu_value);
        [[nodiscard]] bool submitGraphicsFinalizeBatch();
    };

    // ─────────────────────────────────────────────────────────────────────
    //  lookupScene — exported for feature operation handlers
    // ─────────────────────────────────────────────────────────────────────
    LUX_FUNCTION_PUBLIC RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);
    LUX_FUNCTION_PUBLIC RenderContext* lookupRenderContext(void* user_state);
    LUX_FUNCTION_PUBLIC GpuTransferPipeline* lookupTransferPipeline(void* user_state);
    LUX_FUNCTION_PUBLIC void forEachSceneOnServer(void* user_state, void (*fn)(RenderScene&));
} // namespace lux::render
