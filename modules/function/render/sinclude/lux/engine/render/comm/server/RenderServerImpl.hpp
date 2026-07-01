#pragma once

// ─────────────────────────────────────────────────────────────────────────
//  GeneralRenderServer::Impl — full definition (sinclude-visible)
//
//  Moved here from RenderServer.cpp so that UI-module subclasses
//  (UIRenderServer) can directly access Impl members.
// ─────────────────────────────────────────────────────────────────────────

#include <lux/engine/render/comm/server/RenderServer.hpp>   // GeneralRenderServer, FrameReplyBuilder
#include <lux/engine/render/comm/RenderProtocol.hpp>        // FeatureFactory, TypeId
#include <lux/engine/render/comm/RenderTickPipeline.hpp>    // SceneViewBatch (reused per-tick)
#include <lux/engine/render/core/VulkanContext.hpp>          // InstanceContext, DeviceContext, ResourceContext
#include <lux/engine/render/RendererContext.hpp>             // RenderContext
#include <lux/engine/render/FrameStamp.hpp>                  // FrameClock, FrameStamp
#include <lux/engine/render/FrameOrchestrator.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>           // Renderer
#include <lux/engine/render/core/RenderSurface.hpp>          // RenderSurface
#include <lux/engine/render/targets/SwapchainProvider.hpp>     // SwapchainProvider
#include <lux/engine/render/targets/OffscreenImagePool.hpp>    // OffscreenImagePool
#include <lux/engine/render/renderer/FrameDriver.hpp>          // FrameDriver
#include <lux/engine/render/resources/lifecycle/UploadWorkerPool.hpp>  // UploadWorkerPool, TransferCompletion
#include <lux/engine/render/resources/memory/StagingBuffer.hpp>     // StagingBuffer
#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>   // kMaxFramesInFlight
#include <lux/engine/render/core/RenderTypes.hpp>            // kMaxFramesInFlight

#include <lux/cxx/container/SparseSet.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace lux::window { class LuxWindow; }

namespace lux::render
{
    class OffscreenImagePool;
    // FeatureTypeRecord + the feature-type registry moved down to the
    // Renderer-owned FeatureRegistry (renderer/FeatureRegistry.hpp, 阶段 3), so
    // dependency resolution can reach factories. The comm layer registers types
    // INTO it (renderer_->featureRegistry()) and still binds their ops here.

    // ─────────────────────────────────────────────────────────────────────
    //  Handle conversion between cross-thread (R*Handle) and internal (*Handle)
    // ─────────────────────────────────────────────────────────────────────
    template<typename To, typename From>
    To handle_cast(From h) { return To{h.index, h.gen}; }

    // ─────────────────────────────────────────────────────────────────────
    //  GeneralRenderServer::Impl — owns the full Vulkan stack
    // ─────────────────────────────────────────────────────────────────────
    struct GeneralRenderServer::Impl
    {
        // Back-pointer to the owning server — set in constructor.
        // Allows anonymous-namespace handlers to call server public methods.
        GeneralRenderServer* server_{nullptr};

        // Vulkan infrastructure — created lazily by init()
        std::unique_ptr<InstanceContext>  inst_ctx_;
        std::unique_ptr<DeviceContext>    dev_ctx_;
        std::unique_ptr<ResourceContext>  res_ctx_;
        std::shared_ptr<RenderContext>    render_ctx_;
        std::unique_ptr<Renderer>         renderer_;

        // Dispatch
        Dispatcher                        dispatcher;
        // Feature-type registry now lives in renderer_->featureRegistry() (阶段 3).

        // Surface + targets
        RenderSurface                     surface_;
        std::unique_ptr<SwapchainProvider> swapchain_provider_;
        mutable RenderTargetLayout        cached_swapchain_layout_{};
        std::unique_ptr<FrameDriver>      frame_driver_;
        lux::window::LuxWindow*           attached_window_{nullptr};

        // Per-view state — stable scene/view handles; resolved on demand each tick.
        struct OffscreenViewEntry {
            RenderSceneId                      scene_id;
            ViewHandle                         view_id;
            std::unique_ptr<OffscreenImagePool> pool;
            RenderTargetLayout                 layout;
        };
        std::vector<OffscreenViewEntry>   offscreen_views_;

        // Swapchain binding — at most one scene/view bound to the swapchain.
        struct SwapchainBinding {
            RenderSceneId      scene_id{};
            ViewHandle         view_id{};
            RenderTargetLayout layout;
        };
        std::optional<SwapchainBinding>   swapchain_binding_;
        RenderSceneId                     current_bulk_scene_{};

        // Frame timing — single authoritative clock
        FrameOrchestrator                 frame_orchestrator_{2};
        FrameStamp                        current_stamp_{};

        // Per-tick scene/view batch, reused across ticks (cleared at tick start)
        // instead of stack-constructing 3 vectors every frame. MUST be cleared
        // before the offscreen-view loop — stale scene/view pointers would feed
        // endViewFrame after a scene/view was destroyed. (P-5)
        SceneViewBatch                    scene_view_batch_;
        uint32_t                          frames_in_flight_{2};
        bool                              enable_vsync_{true};

        // Async upload worker pool (initialized in Phase 6; used by handlers)
        std::unique_ptr<UploadWorkerPool> upload_pool_;

        // Deferred staging-buffer deletion ring (per FIF slot).
        // Completions drained in tick() push staging here; retired after the
        // corresponding frame's fence has been waited on in beginFrame().
        std::vector<StagingBuffer>        async_deferred_staging_[kMaxFramesInFlight];

        // Deferred offscreen-pool deletion ring (per FIF slot). DestroyScene moves a
        // scene's OffscreenImagePools here (whole-pool GPU resources) instead of
        // freeing them immediately; cleared when the slot's fence is next waited (fif
        // frames later) — same lifetime guarantee as the staging ring. This lets the
        // DestroyScene handler drop its full-device vkDeviceWaitIdle, which used to
        // stall every other scene on each scene teardown. (P0-3)
        std::vector<std::unique_ptr<OffscreenImagePool>>
                                          async_deferred_pools_[kMaxFramesInFlight];

        // Staging buffers finalized during the drain phase.
        // Moved into async_deferred_staging_ at endTickFrame(), AFTER
        // runUploadPhase() has recorded all copies that read them.
        // On ticks that skip the upload phase (minimised window / failed
        // rebuild) they stay here: StagingOnly copy records remain queued
        // until a future runUploadPhase, so the FIF retirement countdown
        // must not start before the copies are actually recorded.
        std::vector<StagingBuffer>        staging_pending_this_tick_;

        // Scratch buffer for drainCompletions() — avoids per-tick allocation.
        static constexpr uint32_t         kMaxDrainBatch = 64;
        TransferCompletion                completion_buf_[kMaxDrainBatch];

        // VRAM budget: set when DEVICE_LOCAL usage > 95%, cleared when < 85%.
        bool                              expansion_suppressed_{false};

        // Deferred resource-upload replies: accumulated during drainCompletions,
        // flushed into the reply builder on the next drainAndDispatch cycle.
        struct DeferredReplyEntry
        {
            uint32_t request_id;
            TransferCompletion::Kind kind;
            uint32_t resource_index; ///< handle.index
            uint32_t resource_gen;   ///< handle.gen
        };
        std::vector<DeferredReplyEntry> pending_deferred_replies_;

        // Completions whose GPU timeline value hasn't been reached yet.
        // Re-checked each tick via non-blocking vkGetSemaphoreCounterValue.
        std::vector<TransferCompletion> pending_completions_;

        // In-flight async readbacks (ReadbackViewAsync). Each entry settles
        // `settle_left` ticks, submits a one-shot image->buffer copy, then is
        // polled each tick; once the fence signals the pixels are copied into
        // dst_ptr and a deferred reply (by request_id) is sent. Lives here so it
        // survives across ticks (unlike the synchronous handleReadbackView).
        struct PendingReadback
        {
            RenderSceneId   scene_id{};
            ViewHandle      view_id{};
            uint64_t        dst_ptr{0};
            uint64_t        dst_capacity{0};
            TargetSlot      slot{TargetSlot::SceneColor}; ///< which output semantic to read (阶段4 P4c)
            uint32_t        request_id{0};
            uint32_t        settle_left{0};   ///< ticks to render before the copy
            uint32_t        deadline{0};      ///< ticks to wait for the fence
            bool            submitted{false};
            bool            done{false};      ///< reply filled, ready to send
            // GPU state — valid only between submit and completion.
            VkBuffer        buf{VK_NULL_HANDLE};
            VmaAllocation   alloc{nullptr};
            void*           mapped{nullptr};
            VkFence         fence{VK_NULL_HANDLE};
            VkCommandBuffer cb{VK_NULL_HANDLE};
            uint32_t        width{0};
            uint32_t        height{0};
            uint32_t        bpp{0};
            VkFormat        format{VK_FORMAT_UNDEFINED};
            uint64_t        needed{0};
            ReadbackViewReply reply{};
        };
        std::vector<PendingReadback> pending_readbacks_;

        /// Callback invoked before a scene is destroyed — lets subclasses
        /// clean up per-scene cached pointers (e.g. UI offscreen views).
        using PreDestroySceneCallback = void(*)(void* extension, RenderSceneId);
        PreDestroySceneCallback pre_destroy_scene_cb_{nullptr};

        /// Opaque pointer for subclass-specific data (e.g. UIRenderServer).
        /// Impl does not own or destroy this — the subclass manages its lifetime.
        void* extension_{nullptr};

        Impl() = default;
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
        void processUploadCompletions(FrameReplyBuilder<64>& replies);

    private:
        // Per-kind completion finalization.
        void finalizeMeshCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        void finalizeTexture2DCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        void finalizeTextureCubeCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
        /// Finalize one completion + accumulate deferred reply + defer staging buffer.
        void finalizeCompletion(TransferCompletion& c, bool needs_qfot, uint32_t src_family, uint32_t dst_family);
    };

    // ─────────────────────────────────────────────────────────────────────
    //  lookupScene — exported for feature operation handlers
    // ─────────────────────────────────────────────────────────────────────
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);
} // namespace lux::render
