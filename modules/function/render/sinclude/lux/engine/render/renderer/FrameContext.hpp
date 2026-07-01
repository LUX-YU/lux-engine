#pragma once
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>
#include <lux/engine/render/FrameStamp.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{

struct RGMultiQueueSubmitInfo;  // forward decl — defined in RGRecorder.hpp

// =============================================================================
// FrameRuntime — render-thread only
// 不可跨线程传递，生命周期从 beginFrame() 到 present()
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
    FrameStamp      stamp          = {};               ///< Authoritative frame identity.
    uint32_t        image_index    = 0;              ///< Swapchain image index.

    VkCommandBuffer primary_cmd    = VK_NULL_HANDLE; ///< Writable — primary command buffer for this frame

    VkImage         present_image  = VK_NULL_HANDLE;
    VkImageView     present_view   = VK_NULL_HANDLE;
    VkExtent2D      present_extent = {};

    /// Swapchain binding for the current frame (filled by SwapchainFrameDriver).
    /// Use as the compose destination. The provider leaves binding.layout null; the
    /// caller sets it per overlay phase before renderSingleView (阶段4 P4d) — hence
    /// this is a MUTABLE handle to the per-frame binding, not a const view.
    /// Valid only within the runFrame() lambda — do NOT hold across frames.
    RenderTargetBinding* present_target = nullptr;

    bool            is_present_frame = false;

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

class  RenderScene;
class  View;


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
    RenderScene*               scene   = nullptr;
    View*                      view    = nullptr; ///< render-thread frame-local, do NOT hold across frames
    const RenderTargetBinding* target  = nullptr;

    uint32_t sort_key = 0;
    bool     enabled  = true;
};

// =============================================================================
// ComposePlan — describes compositing N offscreen targets onto a final target
// =============================================================================

/**
 * @brief One source region in a composite operation.
 */
struct ComposeSource
{
    const RenderTargetBinding* source   = nullptr;
    VkRect2D                   src_rect = {};
    VkRect2D                   dst_rect = {};
    /// Blit filter — set explicitly for HiDPI or non-uniform-scale compositing.
    VkFilter                   filter   = VK_FILTER_LINEAR;
};

/**
 * @brief Declarative description of a full composite pass.
 *
 * Intentionally data-only; barrier and blit details are handled by the
 * composer implementation in FrameExecutor.
 */
struct ComposePlan
{
    const RenderTargetBinding*  destination = nullptr;
    std::span<const ComposeSource> sources;
};

// =============================================================================
// FramePlan — one-frame work aggregate passed to FrameExecutor::execute()
// =============================================================================

/**
 * @brief Aggregates all per-frame render work into a single value object.
 *
 * This is a temporary per-frame value — the caller is responsible for
 * ensuring all span data remains valid for the entire duration of the frame.
 */
struct FramePlan
{
    std::span<RenderScene*>       scenes;
    std::span<DrawRequest>      render_requests;
    std::span<ComposePlan>        compose_plans;
};

} // namespace lux::render
