#pragma once
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{

    struct RGMultiQueueSubmitInfo; // forward decl — defined in RGRecorder.hpp

    // =============================================================================
    // FrameRuntime — render-thread only
    // Not transferable across threads; lifetime runs from beginFrame() to present().
    // =============================================================================

    /**
     * @brief Per-frame backend resource descriptor produced by SwapchainFrameDriver.
     *
     * Carries the low-level handles that every render-thread stage needs:
     * the primary command buffer to record into, the swapchain image to
     * present, and the monotonic frame counters.
     *
     * @note primary_cmd is *writable* — all FrameExecutor phases record GPU
     *       commands into it.  The remaining fields are read-only for the
     *       duration of the frame.
     */
    struct FrameRuntime
    {
        FrameStamp stamp = {};    ///< Authoritative frame identity.
        uint32_t image_index = 0; ///< Swapchain image index.

        VkCommandBuffer primary_cmd = VK_NULL_HANDLE; ///< Writable — primary command buffer for this frame

        VkImage present_image = VK_NULL_HANDLE;
        VkImageView present_view = VK_NULL_HANDLE;
        VkExtent2D present_extent = {};

        /// Swapchain binding for the current frame (filled by SwapchainFrameDriver).
        /// Use as the compose destination. The provider leaves binding.layout null; the
        /// caller sets it per overlay phase before renderSingleView — hence
        /// this is a MUTABLE handle to the per-frame binding, not a const view.
        /// Valid only within the runFrame() lambda — do NOT hold across frames.
        RenderTargetBinding* present_target = nullptr;

        bool is_present_frame = false;

        /// PresentContext 拆层:本帧 acquire 消费的信号量与该 image 的
        /// present 信号量(由 PresentContext::acquire 产出,submit 端直接
        /// wait/signal——FrameDriver 不再自持呈现信号量)。
        VkSemaphore present_acquire_sem = VK_NULL_HANDLE;
        VkSemaphore present_signal_sem = VK_NULL_HANDLE;

        /// Multi-queue submit info produced by the render graph recorder.
        /// Non-null when the graph uses async compute / transfer queues.
        /// Set by Renderer::renderView() after record(); consumed by FrameDriver.
        /// Multiple views may each contribute one submit info in the same tick.
        std::vector<const RGMultiQueueSubmitInfo*> multi_queue_submits;

        /// External (CUDA-produced) timeline waits/signals accumulated from each view's
        /// RGFrameContext this frame; FrameDriver folds them into the GRAPHICS submit.
        /// Empty for normal rendering — the merge is a no-op then (zero regression).
        std::vector<VkSemaphoreSubmitInfo> external_graphics_waits;
        std::vector<VkSemaphoreSubmitInfo> external_graphics_signals;
    };

    // =============================================================================
    // Forward declarations for frame request types
    // =============================================================================

    class RenderScene;
    class View;

    // =============================================================================
    // DrawRequest — describes "render this view of this scene to this target"
    // =============================================================================

    /**
     * @brief Minimum render intent: scene + view + target.
     *
     * Produced by the game/application layer each frame and consumed by
     * FrameExecutor \u2192 Renderer.  Must be constructed and consumed within
     * a single render-thread frame \u2014 do NOT hold across frames.
     *
     * @note View* is a raw pointer valid only while the owning RenderScene is
     *       alive and the view has not been removed.  If the game thread needs
     *       to reference a view, store the uint32_t handle returned by
     *       RenderScene::addView() and convert on the render thread.
     */
    struct DrawRequest
    {
        RenderScene* scene = nullptr;
        View* view = nullptr; ///< render-thread frame-local, do NOT hold across frames
        const RenderTargetBinding* target = nullptr;

        uint32_t sort_key = 0;
        bool enabled = true;
    };

    // (ComposeSource / ComposePlan / FramePlan 已删除 —— 它们描述的是一个不再存在的
    //  FrameExecutor 合成阶段,全仓无任何构造/消费点。合成链现由 RenderServer::tick
    //  的目标遍历承担,见分层设计 §12.7。)

} // namespace lux::render
