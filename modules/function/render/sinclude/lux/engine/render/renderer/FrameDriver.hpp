#pragma once
/**
 * @file FrameDriver.hpp
 * @brief Centralised per-frame synchronisation — owns the single set of fences,
 *        semaphores, and primary command buffers for the entire render tick.
 *
 * One FrameDriver per Renderer.  All offscreen + swapchain graph recordings
 * share the same primary CB → one vkQueueSubmit per frame.
 *
 * Thread model: all methods are render-thread only.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/FrameContext.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/gapi/vk/Fence.hpp>
#include <lux/engine/gapi/vk/Semaphore.hpp>
#include <lux/engine/gapi/vk/CommandBuffer.hpp>
#include <lux/cxx/container/SmallVector.hpp>

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{

class ResourceContext;
class SwapchainProvider;
class SubmissionMerger;   // defined in FrameSubmitMerge.hpp — kept out of this header
struct RGQueueSubmission; // (forward-decl) so external consumers of FrameDriver don't
                          // transitively pull RGRecorder.hpp -> RGCompiledGraph.hpp.

// =============================================================================
// FrameDriver
// =============================================================================

class LUX_FUNCTION_PUBLIC FrameDriver
{
public:
    FrameDriver(ResourceContext& res_ctx, uint32_t frames_in_flight);
    ~FrameDriver();

    FrameDriver(const FrameDriver&)            = delete;
    FrameDriver& operator=(const FrameDriver&) = delete;
    FrameDriver(FrameDriver&&)                 = delete;
    FrameDriver& operator=(FrameDriver&&)      = delete;

    // ── Frame lifecycle ─────────────────────────────────────────────────

    /// Wait fence → handle swapchain rebuild → acquire (if swapchain) →
    /// reset + begin primary CB → return a filled FrameRuntime.
    /// @param frame_index   FIF slot index (0 .. frames_in_flight-1)
    /// @param swapchain     Optional — nullptr for headless / offscreen-only ticks.
    /// @param frame_id      Monotonically increasing ID from the server.
    /// @return FrameRuntime with .primary_cmd ready for recording.
    ///         If the swapchain is minimised or rebuild fails, returns an
    ///         invalid FrameRuntime (check .primary_cmd == VK_NULL_HANDLE).
    [[nodiscard]] FrameRuntime beginFrame(uint32_t frame_index,
                                          uint64_t frame_id,
                                          SwapchainProvider* swapchain);

    /// End primary CB → submit (with semaphore chain if swapchain) → present.
    /// @param rt         The FrameRuntime returned by beginFrame().
    /// @param swapchain  Must be the same pointer passed to beginFrame().
    Expected<void> endFrame(const FrameRuntime& rt,
                            SwapchainProvider* swapchain);

    // ── Utilities ───────────────────────────────────────────────────────

    /// Wait ALL fences — use before resize / rebuild that touches GPU resources
    /// from other FIF slots.
    void waitAllFences();

    [[nodiscard]] uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

private:
    ResourceContext& res_ctx_;
    uint32_t         frames_in_flight_;

    template<typename T>
    using SmallVec = lux::cxx::SmallVector<T, kMaxFramesInFlight>;

    SmallVec<gapi::vk::Fence>          fences_;
    SmallVec<gapi::vk::Semaphore>      acquire_sems_;
    std::vector<gapi::vk::Semaphore>   present_sems_per_image_;
    SmallVec<gapi::vk::CommandBuffer>  cmd_buffers_;

    // Reused per-frame scratch for endFrame() — sized once, then .clear()-reused so
    // the hot submit path performs no heap allocation after warm-up. The merger is
    // held by pointer (forward-declared) so this header doesn't drag RGRecorder ->
    // RGCompiledGraph into render's external consumers (e.g. the ui module).
    std::unique_ptr<SubmissionMerger>      submission_merger_;
    std::vector<const RGQueueSubmission*>  exec_subs_scratch_;
    std::vector<VkSemaphoreSubmitInfo>     retire_waits_scratch_;
    std::vector<VkSemaphoreSubmitInfo>     waits_with_acquire_scratch_;
    std::vector<VkSemaphoreSubmitInfo>     signals_with_external_scratch_;  // graphics signals + external (interop)

    void ensurePresentSemaphoresForSwapchain(SwapchainProvider* swapchain);

    /// Present the acquired swapchain image, mapping OUT_OF_DATE / SUBOPTIMAL to a
    /// rebuild request. Shared by the multi-queue and single-queue submit paths.
    Expected<void> presentImage(SwapchainProvider* swapchain,
                                uint32_t image_index,
                                VkSemaphore present_sem);
};

} // namespace lux::render
