#include <lux/engine/render/renderer/FrameDriver.hpp>
#include <lux/engine/render/renderer/FrameSubmitMerge.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    namespace
    {
        // SubmitMergeKey / SubmitMergeKeyHasher / SubmissionMerger live in
        // FrameSubmitMerge.hpp now (pure, unit-tested merge logic).

        [[nodiscard]] VkQueue selectQueue(ResourceContext &res_ctx, ERGQueueType queue_type)
        {
            auto &dev_ctx = res_ctx.deviceContext();
            switch (queue_type)
            {
            case ERGQueueType::COMPUTE:
                return dev_ctx.asyncComputeQueue();
            case ERGQueueType::TRANSFER:
                return dev_ctx.transferQueue();
            case ERGQueueType::GRAPHICS:
            default:
                return dev_ctx.graphicsQueue();
            }
        }
    } // namespace

    // =============================================================================
    // Construction / Destruction (RAII)
    // =============================================================================
    FrameDriver::FrameDriver(ResourceContext &res_ctx, uint32_t frames_in_flight)
        : res_ctx_(res_ctx), frames_in_flight_(frames_in_flight)
    {
        // Ring-sizing site. RenderContext already gates fif at construction, so this
        // is a release-safe backstop for any direct FrameDriver construction — the
        // per-frame fences/sems/cmd-buffers below are sized to fif and must stay
        // within [1, kMaxFramesInFlight].
        if (frames_in_flight < 1 || frames_in_flight > kMaxFramesInFlight)
            throw std::runtime_error(
                "FrameDriver: frames_in_flight must be in [1, kMaxFramesInFlight].");

        submission_merger_ = std::make_unique<SubmissionMerger>();

        auto &dev = res_ctx_.deviceContext().logicalDevice();

        for (uint32_t i = 0; i < frames_in_flight; ++i)
        {
            // Fences start signaled so the first beginFrame() wait returns immediately.
            fences_.emplace_back(dev); // default ctor → signaled

            acquire_sems_.emplace_back(dev);

            cmd_buffers_.emplace_back(dev, res_ctx_.commandPool());
        }
    }

    FrameDriver::~FrameDriver()
    {
        auto &dev = res_ctx_.deviceContext().logicalDevice();
        dev.waitIdle();

        for (auto &f : fences_)
            f.release(dev);
        for (auto &s : acquire_sems_)
            s.release(dev);
        for (auto &s : present_sems_per_image_)
            s.release(dev);
        for (auto &cb : cmd_buffers_)
            cb.release(dev, res_ctx_.commandPool());
    }

    void FrameDriver::ensurePresentSemaphoresForSwapchain(SwapchainProvider *swapchain)
    {
        if (!swapchain)
            return;

        const uint32_t image_count = swapchain->imageCount();
        if (image_count == 0)
        {
            auto &dev = res_ctx_.deviceContext().logicalDevice();
            for (auto &s : present_sems_per_image_)
                s.release(dev);
            present_sems_per_image_.clear();
            return;
        }

        if (present_sems_per_image_.size() == image_count)
            return;

        auto &dev = res_ctx_.deviceContext().logicalDevice();
        for (auto &s : present_sems_per_image_)
            s.release(dev);
        present_sems_per_image_.clear();
        present_sems_per_image_.reserve(image_count);
        for (uint32_t i = 0; i < image_count; ++i)
            present_sems_per_image_.emplace_back(dev);
    }

    // =============================================================================
    // beginFrame
    // =============================================================================

    FrameRuntime FrameDriver::beginFrame(uint32_t frame_index,
                                         uint64_t frame_id,
                                         SwapchainProvider *swapchain)
    {
        assert(frame_index < frames_in_flight_);
        // Release-safe guard (五-1): an out-of-range slot would index fences_/command
        // pools out of bounds (UB). Every call site passes serial % fif (always in
        // range), so this only fires on a contract violation — fold into range instead
        // of corrupting memory. frames_in_flight_ is >= 1 (RenderContext gate).
        if (frame_index >= frames_in_flight_)
            frame_index %= frames_in_flight_;

        auto &dev = res_ctx_.deviceContext().logicalDevice();
        const auto fi = frame_index;

        // 1. Wait for the previous frame on this FIF slot.
        fences_[fi].wait(dev);

        // 2. Handle pending swapchain rebuild (needs all fences waited).
        if (swapchain && swapchain->needsRebuild())
        {
            waitAllFences();
            auto r = swapchain->rebuild();
            if (!r)
                return {}; // default FrameRuntime — invalid (primary_cmd == VK_NULL_HANDLE)
        }

        // 3. Acquire swapchain image (if present).
        SwapchainProvider::AcquiredImage acquired{};
        if (swapchain)
        {
            ensurePresentSemaphoresForSwapchain(swapchain);
            acquired = swapchain->acquire(acquire_sems_[fi]);
            if (!acquired.valid)
            {
                // Only rebuild if swapchain explicitly reports rebuild need.
                if (!swapchain->needsRebuild())
                    return {};

                waitAllFences();
                auto r = swapchain->rebuild();
                if (!r)
                    return {};

                ensurePresentSemaphoresForSwapchain(swapchain);
                acquired = swapchain->acquire(acquire_sems_[fi]);
                if (!acquired.valid)
                    return {};
            }
        }

        // 4. Reset fence + command buffer + begin recording.
        fences_[fi].reset(dev);
        cmd_buffers_[fi].reset();
        cmd_buffers_[fi].begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

        // 5. Populate FrameRuntime.
        FrameRuntime rt{};
        rt.primary_cmd = cmd_buffers_[fi];
        rt.stamp.serial = frame_id;
        rt.stamp.slot = static_cast<FrameSlot>(fi);
        rt.stamp.frames_in_flight = frames_in_flight_;

        if (swapchain)
        {
            rt.image_index = acquired.image_index;
            rt.stamp.image_index = acquired.image_index;
            rt.present_image = acquired.image;
            rt.present_view = acquired.view;
            rt.present_extent = acquired.extent;
            rt.is_present_frame = true;
        }

        return rt;
    }

    // =============================================================================
    // endFrame
    // =============================================================================

    Expected<void> FrameDriver::endFrame(const FrameRuntime &rt,
                                         SwapchainProvider *swapchain)
    {
        if (rt.primary_cmd == VK_NULL_HANDLE)
            return {};

        const auto fi = rt.stamp.slotIndex();
        VkSemaphore present_sem = VK_NULL_HANDLE;
        if (swapchain)
        {
            const uint32_t image_index = rt.image_index;
            if (image_index >= present_sems_per_image_.size())
                return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
            present_sem = present_sems_per_image_[image_index];
        }

        // 1. End command buffer.
        cmd_buffers_[fi].end();

        // 2. Build submit info.
        VkCommandBuffer cmd = cmd_buffers_[fi];

        if (!rt.multi_queue_submits.empty())
        {
            // Merge submissions from all rendered views by (queue_type, cmd) — avoids
            // O(N^2) linear searches and redundant submits in the hot path. The merger
            // owns reused scratch; the returned ref is valid until its next merge().
            const std::vector<RGQueueSubmission>& merged_subs =
                submission_merger_->merge(rt.multi_queue_submits);

            if (!merged_subs.empty())
            {
                std::vector<const RGQueueSubmission *>& executable_subs = exec_subs_scratch_;
                executable_subs.clear();
                executable_subs.reserve(merged_subs.size());
                bool has_signal_values = false;
                bool has_non_graphics = false;
                for (const auto &sub : merged_subs)
                {
                    if (sub.cmd != VK_NULL_HANDLE)
                        executable_subs.push_back(&sub);
                    if (!sub.signal_semaphores.empty())
                        has_signal_values = true;
                    if (sub.queue_type != ERGQueueType::GRAPHICS)
                        has_non_graphics = true;
                }
                if (executable_subs.empty())
                    return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

                const bool fence_on_last_submission = (!has_signal_values && !swapchain);
                std::vector<VkSemaphoreSubmitInfo>& retire_waits = retire_waits_scratch_;
                retire_waits.clear();
                retire_waits.reserve(16);

                for (size_t si = 0; si < executable_subs.size(); ++si)
                {
                    const auto &sub = *executable_subs[si];

                    const bool is_graphics        = (sub.queue_type == ERGQueueType::GRAPHICS);
                    const bool inject_ext_waits   = is_graphics && !rt.external_graphics_waits.empty();
                    const bool inject_ext_signals = is_graphics && !rt.external_graphics_signals.empty();

                    std::vector<VkSemaphoreSubmitInfo>& waits_with_acquire = waits_with_acquire_scratch_;
                    waits_with_acquire.clear();
                    const VkSemaphoreSubmitInfo *wait_infos = sub.wait_semaphores.data();
                    uint32_t wait_count = static_cast<uint32_t>(sub.wait_semaphores.size());
                    const auto &signals = sub.signal_semaphores;

                    if (is_graphics && (swapchain || inject_ext_waits))
                    {
                        waits_with_acquire.reserve(
                            sub.wait_semaphores.size() + 1u + rt.external_graphics_waits.size());
                        waits_with_acquire.insert(
                            waits_with_acquire.end(),
                            sub.wait_semaphores.begin(),
                            sub.wait_semaphores.end());
                        if (swapchain)
                        {
                            VkSemaphoreSubmitInfo acquire_wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                            acquire_wait.semaphore = acquire_sems_[fi];
                            acquire_wait.value = 0;
                            acquire_wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                            waits_with_acquire.push_back(acquire_wait);
                        }
                        if (inject_ext_waits)   // external (e.g. CUDA) waits before graphics samples
                            waits_with_acquire.insert(
                                waits_with_acquire.end(),
                                rt.external_graphics_waits.begin(),
                                rt.external_graphics_waits.end());

                        wait_infos = waits_with_acquire.data();
                        wait_count = static_cast<uint32_t>(waits_with_acquire.size());
                    }

                    // Submit signals = this submission's signals [+ external interop signals
                    // on the graphics queue]. `signals` (the raw list) is still used for the
                    // retire-wait bookkeeping below, so external signals are kept separate.
                    const VkSemaphoreSubmitInfo* signal_infos = signals.data();
                    uint32_t signal_count = static_cast<uint32_t>(signals.size());
                    if (inject_ext_signals)
                    {
                        std::vector<VkSemaphoreSubmitInfo>& sig = signals_with_external_scratch_;
                        sig.clear();
                        sig.reserve(signals.size() + rt.external_graphics_signals.size());
                        sig.insert(sig.end(), signals.begin(), signals.end());
                        sig.insert(sig.end(),
                                   rt.external_graphics_signals.begin(),
                                   rt.external_graphics_signals.end());
                        signal_infos = sig.data();
                        signal_count = static_cast<uint32_t>(sig.size());
                    }

                    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                    cmd_info.commandBuffer = sub.cmd;

                    VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                    submit2.waitSemaphoreInfoCount = wait_count;
                    submit2.pWaitSemaphoreInfos = wait_infos;
                    submit2.commandBufferInfoCount = 1;
                    submit2.pCommandBufferInfos = &cmd_info;
                    submit2.signalSemaphoreInfoCount = signal_count;
                    submit2.pSignalSemaphoreInfos = signal_infos;

                    VkFence submit_fence = VK_NULL_HANDLE;
                    if (fence_on_last_submission && si + 1 == executable_subs.size())
                        submit_fence = fences_[fi];

                    VkResult sub_res = vkQueueSubmit2(
                        selectQueue(res_ctx_, sub.queue_type),
                        1, &submit2, submit_fence);
                    if (sub_res != VK_SUCCESS)
                        return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

                    retire_waits.insert(retire_waits.end(), signals.begin(), signals.end());
                }

                if (fence_on_last_submission)
                {
                    if (has_non_graphics)
                    {
                        // Fallback path: no timeline graph dependencies were emitted for
                        // async queues. Wait for them explicitly to keep frame retirement safe.
                        auto &dc = res_ctx_.deviceContext();
                        vkQueueWaitIdle(dc.asyncComputeQueue());
                        vkQueueWaitIdle(dc.transferQueue());
                    }
                    return {};
                }

                if (!has_signal_values && has_non_graphics)
                {
                    // No timeline dependencies to join async queues into graphics submit.
                    // Wait explicitly before retire/present submit to keep ordering correct.
                    auto &dc = res_ctx_.deviceContext();
                    vkQueueWaitIdle(dc.asyncComputeQueue());
                    vkQueueWaitIdle(dc.transferQueue());
                }

                std::vector<VkSemaphoreSubmitInfo> retire_signals;
                if (swapchain)
                {
                    VkSemaphoreSubmitInfo present_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                    present_signal.semaphore = present_sem;
                    present_signal.value = 0;
                    present_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                    retire_signals.push_back(present_signal);
                }

                VkSubmitInfo2 retire_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                retire_submit.waitSemaphoreInfoCount = static_cast<uint32_t>(retire_waits.size());
                retire_submit.pWaitSemaphoreInfos = retire_waits.data();
                retire_submit.signalSemaphoreInfoCount = static_cast<uint32_t>(retire_signals.size());
                retire_submit.pSignalSemaphoreInfos = retire_signals.data();

                VkResult retire_res = vkQueueSubmit2(
                    res_ctx_.graphicsQueue(), 1, &retire_submit, fences_[fi]);
                if (retire_res != VK_SUCCESS)
                    return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

                if (swapchain)
                {
                    if (auto r = presentImage(swapchain, rt.image_index, present_sem); !r)
                        return r;
                }

                return {};
            }
        }

        // Interop single-queue path: external timeline waits/signals carry values and
        // need sync2. Taken ONLY when a feature injected external sync; otherwise the
        // existing VkSubmitInfo paths below run byte-for-byte (zero regression).
        if (!rt.external_graphics_waits.empty() || !rt.external_graphics_signals.empty())
        {
            std::vector<VkSemaphoreSubmitInfo>& waits = waits_with_acquire_scratch_;
            waits.clear();
            waits.insert(waits.end(), rt.external_graphics_waits.begin(), rt.external_graphics_waits.end());
            std::vector<VkSemaphoreSubmitInfo>& sigs = signals_with_external_scratch_;
            sigs.clear();
            sigs.insert(sigs.end(), rt.external_graphics_signals.begin(), rt.external_graphics_signals.end());
            if (swapchain)
            {
                VkSemaphoreSubmitInfo aw{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                aw.semaphore = acquire_sems_[fi];
                aw.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                waits.push_back(aw);
                VkSemaphoreSubmitInfo ps{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                ps.semaphore = present_sem;
                ps.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                sigs.push_back(ps);
            }
            VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            ci.commandBuffer = cmd;
            VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit2.waitSemaphoreInfoCount   = static_cast<uint32_t>(waits.size());
            submit2.pWaitSemaphoreInfos      = waits.data();
            submit2.commandBufferInfoCount   = 1;
            submit2.pCommandBufferInfos      = &ci;
            submit2.signalSemaphoreInfoCount = static_cast<uint32_t>(sigs.size());
            submit2.pSignalSemaphoreInfos    = sigs.data();
            VkResult r = vkQueueSubmit2(res_ctx_.graphicsQueue(), 1, &submit2, fences_[fi]);
            if (r != VK_SUCCESS)
                return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
            if (swapchain)
            {
                if (auto pr = presentImage(swapchain, rt.image_index, present_sem); !pr)
                    return pr;
            }
            return {};
        }

        if (swapchain)
        {
            // Swapchain path: wait acquire_sem → draw → signal present_sem + fence.
            VkSemaphore wait_sems[] = {acquire_sems_[fi]};
            VkPipelineStageFlags wait_stages[] = {
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            VkSemaphore signal_sems[] = {present_sem};

            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = wait_sems;
            submit.pWaitDstStageMask = wait_stages;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = signal_sems;

            VkResult r = vkQueueSubmit(res_ctx_.graphicsQueue(), 1, &submit, fences_[fi]);
            if (r != VK_SUCCESS)
                return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

            // Present.
            if (auto r = presentImage(swapchain, rt.image_index, present_sem); !r)
                return r;
        }
        else
        {
            // Offscreen-only path: submit with fence, no semaphores.
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;

            VkResult r = vkQueueSubmit(res_ctx_.graphicsQueue(), 1, &submit, fences_[fi]);
            if (r != VK_SUCCESS)
                return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
        }

        return {};
    }

    Expected<void> FrameDriver::presentImage(SwapchainProvider* swapchain,
                                             uint32_t image_index,
                                             VkSemaphore present_sem)
    {
        VkResult pr = swapchain->present(image_index, present_sem);
        // OUT_OF_DATE / SUBOPTIMAL / SURFACE_LOST are RECOVERABLE swapchain states
        // (window resize, OS drag-drop surface churn) → rebuild, not a fatal error.
        // SURFACE_LOST previously fell into the InternalError branch below, turning
        // a transient surface loss into a frame error.
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR
            || pr == VK_ERROR_SURFACE_LOST_KHR)
            swapchain->markNeedsRebuild();
        else if (pr != VK_SUCCESS)
            return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
        return {};
    }

    // =============================================================================
    // waitAllFences
    // =============================================================================

    void FrameDriver::waitAllFences()
    {
        auto &dev = res_ctx_.deviceContext().logicalDevice();
        for (auto &f : fences_)
            f.wait(dev);
    }

} // namespace lux::render
