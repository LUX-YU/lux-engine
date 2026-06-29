#include <lux/engine/render/graph/RGVulkanRecorder.hpp>

namespace lux::render
{
    // ================================
    // buildMultiQueueSubmit
    // ================================
    void RGVulkanRecorder::buildMultiQueueSubmit(
        RGRecordContext& ctx,
        const RGCompiledGraph& graph,
        VkCommandBuffer cmd,
        VkCommandBuffer compute_cmd,
        VkCommandBuffer transfer_cmd)
    {
        if (!graph.multi_queue_info.has_async_work)
        {
            ctx.multi_queue_submit = {};
            return;
        }

        ctx.multi_queue_submit = {};
        ctx.multi_queue_submit.is_multi_queue = true;

        auto add_semaphore_infos = [&](RGQueueSubmission& sub, const std::vector<uint32_t>& order)
        {
            if (ctx.timeline_semaphore == VK_NULL_HANDLE)
                return;
            for (uint32_t pi : order)
            {
                const auto& cp = graph.compiled_passes[pi];
                for (const auto& sig : cp.signal_dependencies)
                {
                    VkSemaphoreSubmitInfo ssi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
                    ssi.semaphore = ctx.timeline_semaphore;
                    ssi.value = sig.signal_value;
                    ssi.stageMask = sig.signal_stage;
                    sub.signal_semaphores.push_back(ssi);
                }
                for (const auto& w : cp.wait_dependencies)
                {
                    VkSemaphoreSubmitInfo wsi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
                    wsi.semaphore = ctx.timeline_semaphore;
                    wsi.value = w.signal_value;
                    wsi.stageMask = w.wait_stage;
                    sub.wait_semaphores.push_back(wsi);
                }
            }
        };

        {
            RGQueueSubmission gfx{ .queue_type = ERGQueueType::GRAPHICS, .cmd = cmd };
            add_semaphore_infos(gfx, graph.multi_queue_info.graphics_order);
            ctx.multi_queue_submit.submissions.push_back(std::move(gfx));
        }

        if (compute_cmd != VK_NULL_HANDLE)
        {
            vkEndCommandBuffer(compute_cmd);
            RGQueueSubmission comp{ .queue_type = ERGQueueType::COMPUTE, .cmd = compute_cmd };
            add_semaphore_infos(comp, graph.multi_queue_info.compute_order);
            ctx.multi_queue_submit.submissions.push_back(std::move(comp));
        }

        if (transfer_cmd != VK_NULL_HANDLE)
        {
            vkEndCommandBuffer(transfer_cmd);
            RGQueueSubmission xfer{ .queue_type = ERGQueueType::TRANSFER, .cmd = transfer_cmd };
            add_semaphore_infos(xfer, graph.multi_queue_info.transfer_order);
            ctx.multi_queue_submit.submissions.push_back(std::move(xfer));
        }
    }

    // ================================
    // submitFinalBarriers
    // ================================
    void RGVulkanRecorder::submitFinalBarriers(
        VkCommandBuffer cmd,
        const RGCompiledGraph& graph,
        const RGPhysicalResourceTable& physical_resources,
        RGRecordContext& ctx,
        uint32_t frame_index)
    {
        if (graph.prebuilt_final_barriers.empty())
            return;

        auto& barriers = ctx.image_barrier_scratch;
        const auto& src = graph.prebuilt_final_barriers;
        barriers.clear();
        barriers.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i)
        {
            const uint32_t ri = graph.final_patch_resource_idx[i];
            VkImage h = VK_NULL_HANDLE;
            if (ri < ctx.per_frame_images.size()
                && frame_index < ctx.per_frame_images[ri].size()
                && ctx.per_frame_images[ri][frame_index] != VK_NULL_HANDLE)
            {
                h = ctx.per_frame_images[ri][frame_index];
            }
            else if (physical_resources.contains(ri))
            {
                h = reinterpret_cast<VkImage>(physical_resources.at(ri).getHandle(frame_index));
            }
            if (h == VK_NULL_HANDLE)
                continue;
            barriers.push_back(src[i]);
            barriers.back().image = h;
        }
        if (barriers.empty())
            return;

        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}
