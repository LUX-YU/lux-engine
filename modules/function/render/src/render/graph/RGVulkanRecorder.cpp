#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>
#include <lux/engine/render/core/VulkanContext.hpp> // For ResourceContext
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGBarrierUtils.hpp>
#include <lux/engine/render/graph/KernelReplayContext.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/MeshInstanceExtData.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/graph/GpuDrivenMeshConsts.hpp>
#include <array>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace lux::render
{
    // ---------- PassRecordContext::resolveTextureView (out-of-line) ----------
    VkImageView PassRecordContext::resolveTextureView(RGResourceHandle handle) const noexcept
    {
        if (!per_frame_views_)
            return VK_NULL_HANDLE;
        // ping-pong PREVIOUS phase redirects to the peer's earlier-frame copy view.
        uint32_t owner = handle.index;
        uint32_t fidx  = frame.frame_index;
        if (physical_resources_)
        {
            const auto tgt = resolveRingTarget(*physical_resources_, handle.index, frame.frame_index);
            owner = tgt.first;
            fidx  = tgt.second;
        }
        if (owner >= per_frame_views_->size())
            return VK_NULL_HANDLE;
        const auto& fv = (*per_frame_views_)[owner];
        return (fidx < static_cast<uint32_t>(fv.size())) ? fv[fidx] : VK_NULL_HANDLE;
    }

    // ---------- PassRecordContext::resolveTextureView(handle, mip) (out-of-line) ----------
    VkImageView PassRecordContext::resolveTextureView(RGResourceHandle handle, uint32_t mip) const noexcept
    {
        if (!per_frame_views_by_mip_)
            return VK_NULL_HANDLE;
        uint32_t owner = handle.index;
        uint32_t fidx  = frame.frame_index;
        if (physical_resources_)
        {
            const auto tgt = resolveRingTarget(*physical_resources_, handle.index, frame.frame_index);
            owner = tgt.first;
            fidx  = tgt.second;
        }
        if (owner >= per_frame_views_by_mip_->size())
            return VK_NULL_HANDLE;
        const auto& frames = (*per_frame_views_by_mip_)[owner];
        if (fidx >= frames.size())
            return VK_NULL_HANDLE;
        const auto& mips = frames[fidx];
        return (mip < mips.size()) ? mips[mip] : VK_NULL_HANDLE;
    }

    // ---------- PassRecordContext::resolveBufferHandle (out-of-line) ----------
    VkBuffer PassRecordContext::resolveBufferHandle(RGResourceHandle handle) const noexcept
    {
        if (!physical_resources_) return VK_NULL_HANDLE;
        const auto tgt = resolveRingTarget(*physical_resources_, handle.index, frame.frame_index);
        if (auto* pr = physical_resources_->tryGet(tgt.first))
            return reinterpret_cast<VkBuffer>(pr->getHandle(tgt.second));
        return VK_NULL_HANDLE;
    }

    // ---------- KernelReplayContext::resolveBuffer (out-of-line) ----------
    VkBuffer KernelReplayContext::resolveBuffer(uint32_t resource_idx) const
    {
        const auto tgt = resolveRingTarget(physical_resources, resource_idx, frame_ctx.frame_index);
        if (auto* pr = physical_resources.tryGet(tgt.first))
            return reinterpret_cast<VkBuffer>(pr->getHandle(tgt.second));
        return VK_NULL_HANDLE;
    }

    namespace
    {
        struct PipelineVariantBindUser
        {
            VkCommandBuffer cmd{VK_NULL_HANDLE};
            VkPipelineBindPoint bind_point{VK_PIPELINE_BIND_POINT_GRAPHICS};
            BindingState* binding_state{nullptr};
        };

        bool bindPipelineVariantBridge(
            void* raw_user,
            const RGCompiledPass& cpass,
            uint32_t variant_index)
        {
            auto* user = static_cast<PipelineVariantBindUser*>(raw_user);
            if (user == nullptr || user->binding_state == nullptr)
                return false;

            VkPipeline pipeline = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;

            if (!cpass.render.pipeline_variants.empty())
            {
                if (variant_index >= cpass.render.pipeline_variants.size())
                    return false;
                pipeline = cpass.render.pipeline_variants[variant_index];
                if (variant_index < cpass.render.pipeline_variant_layouts.size())
                    layout = cpass.render.pipeline_variant_layouts[variant_index];
            }
            else
            {
                if (variant_index != 0u)
                    return false;
                pipeline = cpass.render.pipeline;
                layout = cpass.render.pipeline_layout;
            }

            if (pipeline == VK_NULL_HANDLE)
                return false;

            BindingState& bs = *user->binding_state;
            if (user->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
            {
                if (bs.last_graphics_pipeline != pipeline)
                    vkCmdBindPipeline(user->cmd, user->bind_point, pipeline);
                bs.last_graphics_pipeline = pipeline;
            }
            else
            {
                if (bs.last_compute_pipeline != pipeline)
                    vkCmdBindPipeline(user->cmd, user->bind_point, pipeline);
                bs.last_compute_pipeline = pipeline;
            }
            if (layout != VK_NULL_HANDLE)
                bs.last_pipeline_layout = layout;
            return true;
        }

        const BarrierProgram::BarrierGroup* resolveBarrierGroupForPass(
            const std::vector<BarrierProgram::BarrierGroup>* groups,
            const std::vector<uint32_t>* group_index_by_pass,
            uint32_t pass_index)
        {
            if (groups == nullptr || group_index_by_pass == nullptr)
                return nullptr;
            if (pass_index >= group_index_by_pass->size())
                return nullptr;

            const uint32_t group_index = (*group_index_by_pass)[pass_index];
            if (group_index == RGCompiledGraph::kInvalidSlotIdx)
                return nullptr;
            if (group_index >= groups->size())
                return nullptr;

            return &(*groups)[group_index];
        }

        VkImage resolveImageHandle(
            const RGRecordContext& record_context,
            const RGPhysicalResourceTable& physical_resources,
            uint32_t resource_idx,
            uint32_t frame_index)
        {
            // ping-pong PREVIOUS phase redirects to the peer's earlier-frame copy.
            const auto tgt = resolveRingTarget(physical_resources, resource_idx, frame_index);
            const uint32_t owner = tgt.first;
            const uint32_t fidx  = tgt.second;

            if (owner < record_context.per_frame_images.size())
            {
                const auto& images = record_context.per_frame_images[owner];
                if (fidx < images.size() && images[fidx] != VK_NULL_HANDLE)
                    return images[fidx];
            }

            if (auto* pr = physical_resources.tryGet(owner))
                return reinterpret_cast<VkImage>(pr->getHandle(fidx));
            return VK_NULL_HANDLE;
        }

        template<typename TSrcImageBarriers, typename TPatchResourceIdx>
        void patchImageBarriers(
            std::vector<VkImageMemoryBarrier2>& out,
            const TSrcImageBarriers& src,
            const TPatchResourceIdx& patch_resource_idx,
            const RGRecordContext& record_context,
            const RGPhysicalResourceTable& physical_resources,
            uint32_t frame_index)
        {
            out.clear(); // capacity already pre-reserved by allocateRecordContext (§1.1)

            const size_t count = std::min(src.size(), patch_resource_idx.size());
            for (size_t i = 0; i < count; ++i)
            {
                VkImage image = resolveImageHandle(
                    record_context,
                    physical_resources,
                    patch_resource_idx[i],
                    frame_index);
                if (image == VK_NULL_HANDLE)
                    continue;

                out.push_back(src[i]);
                out.back().image = image;
            }
        }

        template<typename TSrcBufferBarriers, typename TPatchResourceIdx>
        void patchBufferBarriers(
            std::vector<VkBufferMemoryBarrier2>& out,
            const TSrcBufferBarriers& src,
            const TPatchResourceIdx& patch_resource_idx,
            const RGPhysicalResourceTable& physical_resources,
            uint32_t frame_index)
        {
            out.clear(); // capacity already pre-reserved by allocateRecordContext (§1.1)

            const size_t count = std::min(src.size(), patch_resource_idx.size());
            for (size_t i = 0; i < count; ++i)
            {
                auto* pr = physical_resources.tryGet(patch_resource_idx[i]);
                if (!pr) continue;
                VkBuffer buffer = reinterpret_cast<VkBuffer>(pr->getHandle(frame_index));
                if (buffer == VK_NULL_HANDLE)
                    continue;

                out.push_back(src[i]);
                out.back().buffer = buffer;
            }
        }

        void emitBarrierBatch(
            VkCommandBuffer cmd,
            const std::vector<VkImageMemoryBarrier2>& image_barriers,
            const std::vector<VkBufferMemoryBarrier2>& buffer_barriers)
        {
            if (image_barriers.empty() && buffer_barriers.empty())
                return;

            VkDependencyInfo dep_info{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep_info.imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers.size());
            dep_info.pImageMemoryBarriers = image_barriers.data();
            dep_info.bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers.size());
            dep_info.pBufferMemoryBarriers = buffer_barriers.data();
            vkCmdPipelineBarrier2(cmd, &dep_info);
        }

        // Which pre-pass barrier groups to use this view: first-view groups on
        // the first cross-view pass, subsequent-view groups afterwards. (M17 —
        // was duplicated verbatim in the fast-path record loop and slow-path replay.)
        struct PreBarrierGroupSelection
        {
            const std::vector<BarrierProgram::BarrierGroup>* groups{nullptr};
            const std::vector<uint32_t>*                     group_index_by_pass{nullptr};
        };

        PreBarrierGroupSelection selectPreBarrierGroups(
            const RGCompiledGraph& compiled_graph, const RGFrameContext& frame_ctx)
        {
            const bool need_cross_view_fixup =
                frame_ctx.cross_view_index > 0
                && !compiled_graph.imported_final_states.empty();

            const BarrierProgram* barrier_program =
                compiled_graph.barrier_program.has_value() ? &*compiled_graph.barrier_program : nullptr;

            PreBarrierGroupSelection sel;
            if (barrier_program != nullptr)
            {
                sel.groups = need_cross_view_fixup
                           ? &barrier_program->subsequent_view_barriers
                           : &barrier_program->first_view_barriers;
                sel.group_index_by_pass = need_cross_view_fixup
                           ? &barrier_program->subsequent_view_group_by_pass
                           : &barrier_program->first_view_group_by_pass;
            }
            return sel;
        }

        // Patch + emit one pass's pre-pass barriers. Single source of truth for
        // the fast-path submit_barriers lambda AND the slow-path
        // ECmd::PipelineBarrier replay (M17). pass_index selects the cross-view
        // barrier group. (Split acquire/release phases were removed — see
        // RenderGraphCompiler: NONE/NONE split halves never chained.)
        void emitPassBarriers(
            VkCommandBuffer                                  cmd,
            uint32_t                                         pass_index,
            const RGCompiledPass&                            cpass,
            const std::vector<BarrierProgram::BarrierGroup>* pre_barrier_groups,
            const std::vector<uint32_t>*                     pre_barrier_group_index_by_pass,
            RGRecordContext&                                 record_context,
            const RGPhysicalResourceTable&                   physical_resources,
            uint32_t                                         frame_index)
        {
            const auto& sync = cpass.sync;
            auto& img_scratch = record_context.image_barrier_scratch;
            auto& buf_scratch = record_context.buffer_barrier_scratch;

            const BarrierProgram::BarrierGroup* pre_group = resolveBarrierGroupForPass(
                pre_barrier_groups, pre_barrier_group_index_by_pass, pass_index);

            const auto& src_img = pre_group ? pre_group->image_barriers
                                            : sync.prebuilt_image_barriers;
            const auto& src_img_patch = pre_group ? pre_group->image_patch_resource_idx
                                                  : sync.image_patch_resource_idx;
            patchImageBarriers(img_scratch, src_img, src_img_patch,
                               record_context, physical_resources, frame_index);

            const auto& src_buf = pre_group ? pre_group->buffer_barriers
                                            : sync.prebuilt_buffer_barriers;
            const auto& src_buf_patch = pre_group ? pre_group->buffer_patch_resource_idx
                                                  : sync.buffer_patch_resource_idx;
            patchBufferBarriers(buf_scratch, src_buf, src_buf_patch,
                                physical_resources, frame_index);

            emitBarrierBatch(cmd, img_scratch, buf_scratch);
        }

        bool shouldBindDescriptorSlot(uint32_t bind_mask, uint32_t slot)
        {
            return slot < 32u && (bind_mask & (1u << slot)) != 0u;
        }
    } // namespace

    RGVulkanRecorder::RGVulkanRecorder(const ResourceContext& context, PipelineManager& pipeline_manager)
        : context_(context), pipeline_manager_(pipeline_manager),
          use_dynamic_rendering_(pipeline_manager.useDynamicRendering())
    {
#if !defined(NDEBUG)
        // Load VK_EXT_debug_utils entry points for pass labeling and pipeline
        // object naming.  Both require the extension to be enabled at instance
        // creation (InstanceBuilder::enableDebugReport adds it when debug
        // callbacks are active).  Even if the extension is absent, all three
        // pointers are nullptr and every label/name call becomes a no-op.
        const VkInstance vk_inst = context_.instanceContext().instance().handle();
        fn_begin_debug_label_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(vk_inst, "vkCmdBeginDebugUtilsLabelEXT"));
        fn_end_debug_label_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(vk_inst, "vkCmdEndDebugUtilsLabelEXT"));
        fn_object_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(vk_inst, "vkSetDebugUtilsObjectNameEXT"));
#endif
    }

    Expected<RGRecordContext>
    RGVulkanRecorder::allocateRecordContext(const RGCompiledGraph& compiled_graph, const RGPhysicalResourceTable& physical_resources, VkExtent2D extent, uint32_t frames_in_flight)
    {
        if (!compiled_graph.valid)
            return lux::cxx::unexpected(make_error_code(ERenderError::InvalidCompiledGraph));

        RGRecordContext record_context{};
        // Ensure partially-created Vulkan objects are released on any early return.
        auto cleanup_guard = makeScopeGuard([this, &record_context]() {
            freeRecordContext(record_context);
        });

        record_context.frames_in_flight = frames_in_flight;
        record_context.use_dynamic_rendering = use_dynamic_rendering_;
        // Initialize per-frame binding states
        record_context.binding_states.resize(frames_in_flight);

        if (use_dynamic_rendering_)
        {
            // Dynamic rendering (Vulkan 1.3) — no VkRenderPass or VkFramebuffer needed.
            if (auto err = computeGroupExtents(record_context, compiled_graph, extent))
                return lux::cxx::unexpected(*err);
            if (auto err = preCreateImageViews(record_context, compiled_graph, physical_resources, frames_in_flight))
                return lux::cxx::unexpected(*err);
        }

        // ================================================================
        // A-01: Multi-queue resource allocation (timeline semaphore + async cmd pools/buffers)
        // ================================================================
        if (compiled_graph.multi_queue_info.has_async_work)
        {
            VkDevice device = context_.logicalDevice();
            const auto& dev_ctx = context_.deviceContext();

            // Timeline semaphore for cross-queue synchronization
            VkSemaphoreTypeCreateInfo timeline_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            timeline_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            timeline_ci.initialValue  = 0;

            VkSemaphoreCreateInfo sem_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            sem_ci.pNext = &timeline_ci;

            if (vkCreateSemaphore(device, &sem_ci, context_.instanceContext().allocator(),
                                  &record_context.timeline_semaphore) != VK_SUCCESS)
                return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

            // Async compute command pool + per-frame command buffers
            if (!compiled_graph.multi_queue_info.compute_order.empty())
            {
                VkCommandPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_ci.queueFamilyIndex = dev_ctx.asyncComputeQueueFamilyIndex();

                if (vkCreateCommandPool(device, &pool_ci, context_.instanceContext().allocator(),
                                        &record_context.compute_cmd_pool) != VK_SUCCESS)
                    return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

                record_context.compute_cmd_bufs.resize(frames_in_flight, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo alloc_ci{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                alloc_ci.commandPool        = record_context.compute_cmd_pool;
                alloc_ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_ci.commandBufferCount = frames_in_flight;

                if (vkAllocateCommandBuffers(device, &alloc_ci, record_context.compute_cmd_bufs.data()) != VK_SUCCESS)
                    return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));
            }

            // Async transfer command pool + per-frame command buffers
            if (!compiled_graph.multi_queue_info.transfer_order.empty())
            {
                VkCommandPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_ci.queueFamilyIndex = dev_ctx.transferQueueFamilyIndex();

                if (vkCreateCommandPool(device, &pool_ci, context_.instanceContext().allocator(),
                                        &record_context.transfer_cmd_pool) != VK_SUCCESS)
                    return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

                record_context.transfer_cmd_bufs.resize(frames_in_flight, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo alloc_ci{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                alloc_ci.commandPool        = record_context.transfer_cmd_pool;
                alloc_ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_ci.commandBufferCount = frames_in_flight;

                if (vkAllocateCommandBuffers(device, &alloc_ci, record_context.transfer_cmd_bufs.data()) != VK_SUCCESS)
                    return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));
            }
        }

        // ========== Allocate transient descriptor sets ==========
        const auto& tds_descs = compiled_graph.original_graph.transient_descriptor_sets;
        if (!tds_descs.empty())
        {
            VkDevice device = context_.logicalDevice();
            // Count total sets needed: num_descs * frames_in_flight
            const uint32_t total_sets = static_cast<uint32_t>(tds_descs.size()) * frames_in_flight;

            // Gather pool sizes from all descriptor writes
            std::unordered_map<VkDescriptorType, uint32_t> type_counts;
            for (const auto& desc : tds_descs)
                for (const auto& w : desc.writes)
                    type_counts[convertDescriptorType(w.descriptor_type)] += frames_in_flight;

            std::vector<VkDescriptorPoolSize> pool_sizes;
            pool_sizes.reserve(type_counts.size());
            for (auto& [dt, count] : type_counts)
                pool_sizes.push_back({ dt, count });

            VkDescriptorPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pool_ci.maxSets       = total_sets;
            pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
            pool_ci.pPoolSizes    = pool_sizes.data();

            if (vkCreateDescriptorPool(device, &pool_ci, context_.instanceContext().allocator(),
                                       &record_context.transient_ds_pool) != VK_SUCCESS)
                return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

            // Allocate sets and write descriptors
            record_context.transient_descriptor_sets.resize(tds_descs.size());
            for (size_t di = 0; di < tds_descs.size(); ++di)
            {
                auto& per_frame = record_context.transient_descriptor_sets[di];
                per_frame.resize(frames_in_flight, VK_NULL_HANDLE);

                // Allocate all frames at once
                std::vector<VkDescriptorSetLayout> layouts(frames_in_flight, tds_descs[di].layout);
                VkDescriptorSetAllocateInfo alloc_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                alloc_ci.descriptorPool     = record_context.transient_ds_pool;
                alloc_ci.descriptorSetCount = frames_in_flight;
                alloc_ci.pSetLayouts        = layouts.data();

                if (vkAllocateDescriptorSets(device, &alloc_ci, per_frame.data()) != VK_SUCCESS)
                    return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

                // Write descriptors for each frame
                for (uint32_t fi = 0; fi < frames_in_flight; ++fi)
                {
                    std::vector<VkWriteDescriptorSet>   vk_writes;
                    std::vector<VkDescriptorImageInfo>  image_infos;
                    std::vector<VkDescriptorBufferInfo> buffer_infos;
                    vk_writes.reserve(tds_descs[di].writes.size());
                    image_infos.reserve(tds_descs[di].writes.size());
                    buffer_infos.reserve(tds_descs[di].writes.size());

                    for (const auto& w : tds_descs[di].writes)
                    {
                        const VkDescriptorType vk_dtype = convertDescriptorType(w.descriptor_type);
                        VkWriteDescriptorSet vw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                        vw.dstSet          = per_frame[fi];
                        vw.dstBinding      = w.binding;
                        vw.descriptorCount = 1;
                        vw.descriptorType  = vk_dtype;

                        const auto ri = w.resource.index;
                        switch (vk_dtype)
                        {
                        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                        {
                            // ping-pong PREVIOUS redirects to the peer's earlier-frame copy view.
                            const auto img_tgt = resolveRingTarget(physical_resources, ri, fi);
                            VkImageView dview = VK_NULL_HANDLE;
                            if (w.mip_level != ~0u)
                            {
                                // single per-mip view (e.g. HZB downsample src/dst level)
                                const auto& by_mip = record_context.per_frame_views_by_mip;
                                if (img_tgt.first < by_mip.size()
                                    && img_tgt.second < by_mip[img_tgt.first].size()
                                    && w.mip_level < by_mip[img_tgt.first][img_tgt.second].size())
                                    dview = by_mip[img_tgt.first][img_tgt.second][w.mip_level];
                            }
                            else
                            {
                                dview = record_context.per_frame_views[img_tgt.first][img_tgt.second];
                            }
                            VkDescriptorImageInfo ii{};
                            ii.imageView   = dview;
                            ii.imageLayout = convertImageLayout(w.image_layout);
                            ii.sampler     = w.sampler;
                            image_infos.push_back(ii);
                            vw.pImageInfo = &image_infos.back();
                            break;
                        }
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                        {
                            const auto buf_tgt = resolveRingTarget(physical_resources, ri, fi);
                            VkDescriptorBufferInfo bi{};
                            bi.buffer = reinterpret_cast<VkBuffer>(physical_resources.at(buf_tgt.first).getHandle(buf_tgt.second));
                            bi.offset = 0;
                            bi.range  = VK_WHOLE_SIZE;
                            buffer_infos.push_back(bi);
                            vw.pBufferInfo = &buffer_infos.back();
                            break;
                        }
                        default:
                            break;
                        }
                        vk_writes.push_back(vw);
                    }
                    if (!vk_writes.empty())
                        vkUpdateDescriptorSets(device, static_cast<uint32_t>(vk_writes.size()),
                                               vk_writes.data(), 0, nullptr);
                }
            }
        }

        // §1.1: Pre-size scratch buffers to max barrier count across all passes.
        // After this, patchImageBarriers/patchBufferBarriers only need clear() (no reserve/realloc).
        {
            uint32_t max_img = 0, max_buf = 0;
            for (const auto& cp : compiled_graph.compiled_passes)
            {
                max_img = std::max(max_img, static_cast<uint32_t>(cp.sync.prebuilt_image_barriers.size()));
                max_buf = std::max(max_buf, static_cast<uint32_t>(cp.sync.prebuilt_buffer_barriers.size()));
            }
            if (compiled_graph.barrier_program.has_value())
            {
                auto scan_groups = [&](const std::vector<BarrierProgram::BarrierGroup>& groups)
                {
                    for (const auto& g : groups)
                    {
                        max_img = std::max(max_img, static_cast<uint32_t>(g.image_barriers.size()));
                        max_buf = std::max(max_buf, static_cast<uint32_t>(g.buffer_barriers.size()));
                    }
                };
                scan_groups(compiled_graph.barrier_program->first_view_barriers);
                scan_groups(compiled_graph.barrier_program->subsequent_view_barriers);
                scan_groups(compiled_graph.barrier_program->final_barriers);
            }
            record_context.image_barrier_scratch.reserve(max_img);
            record_context.buffer_barrier_scratch.reserve(max_buf);
        }

        cleanup_guard.dismiss();
        return record_context;
    }

    bool RGVulkanRecorder::deallocateRecordContext(RGRecordContext& record_context)
    {
        freeRecordContext(record_context);
        return true;
    }

    void RGVulkanRecorder::freeRecordContext(RGRecordContext& record_context)
    {
        destroyImageViews(record_context);

        // A-01: Destroy multi-queue resources
        VkDevice device = context_.logicalDevice();
        const VkAllocationCallbacks* alloc = context_.instanceContext().allocator();

        if (record_context.compute_cmd_pool != VK_NULL_HANDLE)
        {
            // Command buffers are freed implicitly when the pool is destroyed
            vkDestroyCommandPool(device, record_context.compute_cmd_pool, alloc);
            record_context.compute_cmd_pool = VK_NULL_HANDLE;
            record_context.compute_cmd_bufs.clear();
        }
        if (record_context.transfer_cmd_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, record_context.transfer_cmd_pool, alloc);
            record_context.transfer_cmd_pool = VK_NULL_HANDLE;
            record_context.transfer_cmd_bufs.clear();
        }
        if (record_context.timeline_semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device, record_context.timeline_semaphore, alloc);
            record_context.timeline_semaphore = VK_NULL_HANDLE;
        }

        // Destroy transient descriptor set pool (sets freed implicitly)
        if (record_context.transient_ds_pool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, record_context.transient_ds_pool, alloc);
            record_context.transient_ds_pool = VK_NULL_HANDLE;
            record_context.transient_descriptor_sets.clear();
        }
    }

    // ================================
    // fillAttachmentInfos: shared helper for parallel and serial dynamic-rendering paths.
    // Fills VkRenderingAttachmentInfo arrays from the pre-created view table (per_frame_views)
    // without calling private member functions, keeping it a pure free function.
    // ================================
    static void fillAttachmentInfos(
        const RGCompiledGraph&   compiled_graph,
        const RGRecordContext&   record_ctx,
        const RGCompiledPass&    sub_cpass,
        const RGRenderPassGroup& group,
        uint32_t                 frame_index,
        VkAttachmentLoadOp       color_op,
        VkAttachmentLoadOp       depth_op,
        std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments>& out_color,
        VkRenderingAttachmentInfo& out_depth)
    {
        for (uint32_t ci = 0; ci < group.key.color_count; ++ci)
        {
            out_color[ci] = {};
            out_color[ci].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            out_color[ci].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            out_color[ci].loadOp      = color_op;
            out_color[ci].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            out_color[ci].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        }
        out_depth = {};
        out_depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        out_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        out_depth.loadOp      = depth_op;
        out_depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        out_depth.clearValue.depthStencil = {1.0f, 0};

        if (!sub_cpass.pass) return;
        uint32_t color_idx = 0;
        for (const auto& tex_ref : sub_cpass.pass->textures)
        {
            VkImageView view = VK_NULL_HANDLE;
            if (tex_ref.resource.index < record_ctx.per_frame_views.size())
            {
                const auto& fv = record_ctx.per_frame_views[tex_ref.resource.index];
                if (frame_index < fv.size())
                    view = fv[frame_index];
            }
            if (view == VK_NULL_HANDLE) continue;

            if (tex_ref.role == lux::common::ETextureRole::COLOR_ATTACHMENT &&
                color_idx < group.key.color_count)
                out_color[color_idx++].imageView = view;
            else if (tex_ref.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                out_depth.imageView = view;
        }
    }

    // ================================
    // refreshDynamicImportedResources: pre-record dynamic handle refresh
    // ================================
    void RGVulkanRecorder::refreshDynamicImportedResources(
        RGResourceState& state,
        const RGCompiledGraph& compiled_graph)
    {
        // Refresh dynamic imported buffer handles so growth/reallocation in
        // upload phase is visible to the upcoming record() call.
        // Image getters are intentionally skipped: VkImage changes require
        // view recreation and must go through graph invalidation/reallocation.
        for (uint32_t ri : compiled_graph.dynamic_external_resources)
        {
            auto* phys_ptr = state.physical_resources.tryGet(ri);
            if (!phys_ptr)
                continue;

            auto& phys = *phys_ptr;
            if (!phys.buffer_getter)
                continue;

            std::array<VkBuffer, 8> new_buffers{};
            const uint32_t count = phys.buffer_getter(
                new_buffers.data(), static_cast<uint32_t>(new_buffers.size()));
            if (count == 0)
                continue;

            // §1.2: Short-circuit when handles are unchanged (common steady-state path).
            if (count == static_cast<uint32_t>(phys.physical_handles.size()))
            {
                bool same = true;
                for (uint32_t i = 0; i < count && same; ++i)
                    same = (phys.physical_handles[i] == reinterpret_cast<uintptr_t>(new_buffers[i]));
                if (same) continue;
            }

            phys.physical_handles.resize(count);
            for (uint32_t i = 0; i < count; ++i)
                phys.physical_handles[i] = reinterpret_cast<uintptr_t>(new_buffers[i]);
        }
    }

    struct RGVulkanRecorder::ExecutionReplayState
    {
        BindingState* binding_state{nullptr};
        uint32_t current_pass{UINT32_MAX};
        VkPipelineBindPoint current_bp{VK_PIPELINE_BIND_POINT_GRAPHICS};
        VkPipelineLayout current_layout{VK_NULL_HANDLE};
        bool skip_next_draw{false};
    };

    // ================================
    // record: record command buffer for each frame
    // ================================
    void RGVulkanRecorder::record(RGResourceState& state, const RGCompiledGraph& compiled_graph, const RGFrameContext& frame_ctx, VkCommandBuffer target_cmd, ResourceRegistryBase* gpu_mgr)
    {
        auto& record_context = state.record_ctx;
        record_context.physical_resources_ptr = &state.physical_resources;
        // Inject per-slot image/view overrides into the per-frame override tables.
        // The compiled graph remains immutable; barrier and rendering code consults
        // per_frame_images / per_frame_views before falling back to physical_resources.
        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            const uint32_t ri = compiled_graph.slot_resource_idx[si];
            if (ri == RGCompiledGraph::kInvalidSlotIdx) continue;

            const auto& binding = frame_ctx.imported_slots[si];
            if (binding.image == VK_NULL_HANDLE) continue;

            if (ri < record_context.per_frame_images.size() &&
                frame_ctx.frame_index < record_context.per_frame_images[ri].size())
                record_context.per_frame_images[ri][frame_ctx.frame_index] = binding.image;

            if (ri < record_context.per_frame_views.size() &&
                frame_ctx.frame_index < record_context.per_frame_views[ri].size())
                record_context.per_frame_views[ri][frame_ctx.frame_index] = binding.view;
        }

        // Reset per-frame binding state so stale handles from the previous frame
        // are not mistakenly treated as "already bound" during this frame's record().
        if (frame_ctx.frame_index < record_context.binding_states.size())
            record_context.binding_states[frame_ctx.frame_index].reset();

    #if !defined(NDEBUG)
        debug_named_pipelines_.clear();
    #endif

        VkCommandBuffer cmd = target_cmd;

        // ================================================================
        // A-01: Multi-queue command buffer setup
        // ================================================================
        const bool is_multi_queue = compiled_graph.multi_queue_info.has_async_work;
        VkCommandBuffer compute_cmd  = VK_NULL_HANDLE;
        VkCommandBuffer transfer_cmd = VK_NULL_HANDLE;

        if (is_multi_queue)
        {
            const VkCommandBufferBeginInfo begin_info{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };

            if (frame_ctx.frame_index < record_context.compute_cmd_bufs.size())
            {
                compute_cmd = record_context.compute_cmd_bufs[frame_ctx.frame_index];
                vkResetCommandBuffer(compute_cmd, 0);
                vkBeginCommandBuffer(compute_cmd, &begin_info);
            }
            if (frame_ctx.frame_index < record_context.transfer_cmd_bufs.size())
            {
                transfer_cmd = record_context.transfer_cmd_bufs[frame_ctx.frame_index];
                vkResetCommandBuffer(transfer_cmd, 0);
                vkBeginCommandBuffer(transfer_cmd, &begin_info);
            }
        }

        // ================================================================
        // Path selection: fast path (ExecutionProgram) vs slow path (recorder lambdas)
        // ================================================================
        // Dynamic rendering forbids barriers inside an active rendering scope.
        // The compiler verifies that no PipelineBarrier falls between
        // BeginRendering / EndRendering and sets dynamic_rendering_compatible
        // accordingly, allowing the fast path when safe.
        const bool allow_full_fast =
            compiled_graph.hasFullFastPath()
            && (!record_context.use_dynamic_rendering
                || compiled_graph.execution_program->dynamic_rendering_compatible);
        if (allow_full_fast)
        {
            executeFast(cmd, compiled_graph, record_context, frame_ctx,
                        state.physical_resources, gpu_mgr);

            // Finalize async command buffers (fast path uses primary cmd only for now).
            buildMultiQueueSubmit(record_context, compiled_graph, cmd, compute_cmd, transfer_cmd);
        }
        else
        {

        // Marks passes that have been processed, prevents re-processing Subpasses within a Group when iterating execution_order
        // Reuse scratch buffer from record_context to avoid per-frame heap allocation
        auto& processed_passes = record_context.processed_passes_scratch;
        processed_passes.assign(compiled_graph.compiled_passes.size(), 0);

        ExecutionReplayState mixed_replay_state{};
        if (compiled_graph.hasFastPath())
            mixed_replay_state.binding_state = &record_context.binding_states[frame_ctx.frame_index];

        auto compiled_span_for_pass = [&](uint32_t pass_index)
            -> const ExecutionProgram::PassCommandSpan*
        {
            if (!compiled_graph.execution_program.has_value())
                return nullptr;
            const auto& spans = compiled_graph.execution_program->pass_spans;
            if (pass_index >= spans.size())
                return nullptr;
            return spans[pass_index].valid ? &spans[pass_index] : nullptr;
        };

        const auto& physical_resources = state.physical_resources;

        // Helper lambda: submit pre-computed compile-time barriers for a pass.
        // A-01: accepts explicit VkCommandBuffer to support per-queue barrier submission.

        // ================================================================
        // Cross-view barrier fixup (multi-view same-scene recording)
        // ================================================================
        // When cross_view_index > 0 the same compiled graph is being recorded
        // again into the same command buffer.  Imported resources are NOT in
        // their declared initial state — they are in the deterministic final
        // state left by the previous view's recording.  We track which
        // imported resources have had their first-touch barrier patched so
        // each is fixed up exactly once.
        const PreBarrierGroupSelection pre_bsel = selectPreBarrierGroups(compiled_graph, frame_ctx);

        auto submit_barriers = [&](VkCommandBuffer barrier_cmd, uint32_t pass_index, const RGCompiledPass& cpass) {
            emitPassBarriers(barrier_cmd, pass_index, cpass,
                             pre_bsel.groups, pre_bsel.group_index_by_pass,
                             record_context, physical_resources, frame_ctx.frame_index);
        };

        // Per-graph execution order dump removed (was [RGDbg])

        // Iterate execution order
        for (uint32_t pass_idx : compiled_graph.execution_order)
        {
            // If this Pass has already been processed (as part of a Group), skip it
            if (processed_passes[pass_idx]) continue;

            const RGCompiledPass& cpass = compiled_graph.compiled_passes[pass_idx];

            // Conditional pass execution check (runtime lambda only; feature
            // enable/disable is handled at compile time via graph invalidation).
            if (cpass.condition && !(*cpass.condition)()) {
                processed_passes[pass_idx] = 1;
                continue;
            }

            // ===========================
            // Case 1: Compute / Async Compute Pass
            // ===========================
            if (cpass.pass->type == ERGPassType::COMPUTE || cpass.pass->type == ERGPassType::ASYNC_COMPUTE)
            {
                // A-01: Route to the correct command buffer based on queue assignment
                VkCommandBuffer pass_cmd = cmd;
                if (cpass.queue_type == ERGQueueType::COMPUTE && compute_cmd != VK_NULL_HANDLE)
                    pass_cmd = compute_cmd;

                submit_barriers(pass_cmd, pass_idx, cpass);
                if (const auto* span = compiled_span_for_pass(pass_idx))
                {
                    mixed_replay_state.current_pass = UINT32_MAX;
                    mixed_replay_state.current_layout = VK_NULL_HANDLE;
                    replayExecutionRange(pass_cmd, compiled_graph, record_context, frame_ctx,
                                         state.physical_resources, gpu_mgr,
                                         mixed_replay_state,
                                         span->first_command,
                                         span->one_past_last_command,
                                         false,
                                         true);
                }
                else
                {
                    recordPassContent(pass_cmd, cpass, compiled_graph, record_context, frame_ctx,
                                      gpu_mgr, VK_PIPELINE_BIND_POINT_COMPUTE, {});
                }
                processed_passes[pass_idx] = 1;
            }
            // ===========================
            // Case 1b: Transfer / Async Transfer Pass (A-01)
            // ===========================
            else if (cpass.pass->type == ERGPassType::TRANSFER || cpass.pass->type == ERGPassType::ASYNC_TRANSFER)
            {
                VkCommandBuffer pass_cmd = cmd;
                if (cpass.queue_type == ERGQueueType::TRANSFER && transfer_cmd != VK_NULL_HANDLE)
                    pass_cmd = transfer_cmd;

                submit_barriers(pass_cmd, pass_idx, cpass);
                if (const auto* span = compiled_span_for_pass(pass_idx))
                {
                    mixed_replay_state.current_pass = UINT32_MAX;
                    mixed_replay_state.current_layout = VK_NULL_HANDLE;
                    replayExecutionRange(pass_cmd, compiled_graph, record_context, frame_ctx,
                                         state.physical_resources, gpu_mgr,
                                         mixed_replay_state,
                                         span->first_command,
                                         span->one_past_last_command,
                                         false,
                                         true);
                }
                else if (cpass.pass->recorder || cpass.pass->kernel_fn)
                    recordPassContent(pass_cmd, cpass, compiled_graph, record_context, frame_ctx,
                                      gpu_mgr, VK_PIPELINE_BIND_POINT_COMPUTE, {});
                processed_passes[pass_idx] = 1;
            }
            // ===========================
            // Case 2: Graphics Pass (RenderPass Group)
            // ===========================
            else if (cpass.pass->type == ERGPassType::GRAPHICS)
            {
                uint32_t group_idx = compiled_graph.render_pass_layout.pass_to_group[pass_idx];

                if (group_idx == std::numeric_limits<uint32_t>::max()) {
                    continue;
                }

                const auto& group = compiled_graph.render_pass_layout.groups[group_idx];

                // F: Barriers are submitted per-pass, skipping conditionally-disabled passes.

                // Determine loadOps for this group's attachments (pre-computed at compile time).
                const VkAttachmentLoadOp group_color_op = group.color_load_op;
                const VkAttachmentLoadOp group_depth_op = group.depth_load_op;

                if (record_context.use_dynamic_rendering)
                {
                    // ===== Dynamic Rendering serial path (Vulkan 1.3) =====
                    // Each pass gets its own vkCmdBeginRendering scope.
                    // The first non-skipped pass uses the group's pre-computed loadOps;
                    // subsequent passes in the same group LOAD to accumulate prior output.
                    bool serial_first_pass = true;
                    for (size_t i = 0; i < group.passes.size(); ++i)
                    {
                        uint32_t sub_pass_logic_idx = group.passes[i].pass_index;
                        const RGCompiledPass& sub_cpass = compiled_graph.compiled_passes[sub_pass_logic_idx];

                        bool skip_subpass = false;
                        if (sub_cpass.condition && !(*sub_cpass.condition)()) skip_subpass = true;

                        if (!skip_subpass) {
                            const VkAttachmentLoadOp col_op = serial_first_pass ? group_color_op : VK_ATTACHMENT_LOAD_OP_LOAD;
                            const VkAttachmentLoadOp dep_op = serial_first_pass ? group_depth_op : VK_ATTACHMENT_LOAD_OP_LOAD;
                            serial_first_pass = false;

                            std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments> color_attachments{};
                            VkRenderingAttachmentInfo depth_attachment{};
                            fillAttachmentInfos(compiled_graph, record_context, sub_cpass, group,
                                                frame_ctx.frame_index, col_op, dep_op,
                                                color_attachments, depth_attachment);

                            VkRenderingInfo rendering_info{};
                            rendering_info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                            rendering_info.renderArea.extent    = record_context.group_extents[group_idx];
                            rendering_info.layerCount           = record_context.group_layer_counts[group_idx];
                            rendering_info.colorAttachmentCount = group.key.color_count;
                            rendering_info.pColorAttachments    = group.key.color_count > 0 ? color_attachments.data() : nullptr;
                            rendering_info.pDepthAttachment     = (group.key.depth_stencil_format != VK_FORMAT_UNDEFINED && depth_attachment.imageView != VK_NULL_HANDLE) ? &depth_attachment : nullptr;

                            submit_barriers(cmd, sub_pass_logic_idx, sub_cpass);
                            vkCmdBeginRendering(cmd, &rendering_info);
                            if (const auto* span = compiled_span_for_pass(sub_pass_logic_idx))
                            {
                                mixed_replay_state.current_pass = UINT32_MAX;
                                mixed_replay_state.current_layout = VK_NULL_HANDLE;
                                replayExecutionRange(cmd, compiled_graph, record_context, frame_ctx,
                                                     state.physical_resources, gpu_mgr,
                                                     mixed_replay_state,
                                                     span->first_command,
                                                     span->one_past_last_command,
                                                     false,
                                                     false);
                            }
                            else
                            {
                                recordPassContent(cmd, sub_cpass, compiled_graph, record_context, frame_ctx,
                                                  gpu_mgr, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  record_context.group_extents[group_idx]);
                            }
                            vkCmdEndRendering(cmd);
                        }

                        processed_passes[sub_pass_logic_idx] = 1;
                    }
                }
                // Legacy VkRenderPass/VkFramebuffer path removed.
                // The engine requires dynamic rendering (Vulkan 1.3 / VK_KHR_dynamic_rendering).
            }
        }

        // Finalize async command buffers and populate multi-queue submit info.
        buildMultiQueueSubmit(record_context, compiled_graph, cmd, compute_cmd, transfer_cmd);

        } // end slow path

        // Emit graph-export barriers (e.g., swapchain PRESENT_SRC_KHR transition).
        submitFinalBarriers(cmd, compiled_graph, state.physical_resources, record_context,
                            frame_ctx.frame_index);

        // Clear injected external slot views AND images so they are not used by a
        // later record(). Imported slot views/images (e.g. from OffscreenRenderTarget)
        // are owned by the calling code — both are re-injected on every record(), so
        // zeroing them here is safe. Clearing the view but NOT the image left a stale
        // VkImage override that resolveImageHandle() (which checks per_frame_images
        // before the physical table) could feed into barriers after the external
        // image was torn down — emitting a transition against a dead image. (P1#19)
        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            const uint32_t ri = compiled_graph.slot_resource_idx[si];
            if (ri == RGCompiledGraph::kInvalidSlotIdx) continue;
            if (ri < record_context.per_frame_views.size() &&
                frame_ctx.frame_index < record_context.per_frame_views[ri].size())
                record_context.per_frame_views[ri][frame_ctx.frame_index] = VK_NULL_HANDLE;
            if (ri < record_context.per_frame_images.size() &&
                frame_ctx.frame_index < record_context.per_frame_images[ri].size())
                record_context.per_frame_images[ri][frame_ctx.frame_index] = VK_NULL_HANDLE;
        }
    }

    // ================================================================
    // executeFast — Phase 2 fast-path executor
    // ================================================================
    // Linearly replays the pre-compiled ExecutionProgram, bypassing
    // recorder-lambda dispatch entirely.  Only used when all passes
    // declare a kernel and no runtime conditions exist.
    void RGVulkanRecorder::executeFast(
        VkCommandBuffer cmd,
        const RGCompiledGraph& compiled_graph,
        RGRecordContext& record_context,
        const RGFrameContext& frame_ctx,
        const RGPhysicalResourceTable& physical_resources,
        ResourceRegistryBase* gpu_mgr)
    {
        if (!compiled_graph.execution_program.has_value())
            return;

        ExecutionReplayState replay_state{};
        replay_state.binding_state = &record_context.binding_states[frame_ctx.frame_index];

        replayExecutionRange(
            cmd,
            compiled_graph,
            record_context,
            frame_ctx,
            physical_resources,
            gpu_mgr,
            replay_state,
            0u,
            static_cast<uint32_t>(compiled_graph.execution_program->commands.size()),
            true,
            true);
    }

    void RGVulkanRecorder::replayExecutionRange(
        VkCommandBuffer cmd,
        const RGCompiledGraph& compiled_graph,
        RGRecordContext& record_context,
        const RGFrameContext& frame_ctx,
        const RGPhysicalResourceTable& physical_resources,
        ResourceRegistryBase* gpu_mgr,
        ExecutionReplayState& replay_state,
        uint32_t first_command,
        uint32_t one_past_last_command,
        bool replay_barriers,
        bool replay_render_scope)
    {
        using ECmd = ExecutionProgram::Command::EType;
        if (!compiled_graph.execution_program.has_value() || replay_state.binding_state == nullptr)
            return;

        // skip_next_draw is a per-shadow-lane flag (set by kDrawLaneSetup, consumed
        // by that lane's DrawIndexedIndirectCount, and intentionally NOT self-cleared
        // so it skips the WHOLE inactive lane — C-7). The same replay_state is reused
        // across passes, and the generic DrawIndexedIndirectCount handler is shared
        // with the MAIN mesh pass, which emits no kDrawLaneSetup to re-arm it. So an
        // inactive LAST shadow lane would leave the flag set and silently skip every
        // main-pass draw (nothing renders). Reset it at each pass boundary — this
        // replay covers exactly one pass's command range. (C-7 bleed fix)
        replay_state.skip_next_draw = false;

        const auto& program = *compiled_graph.execution_program;
        const uint32_t command_count = static_cast<uint32_t>(program.commands.size());
        const uint32_t begin = std::min(first_command, command_count);
        const uint32_t end = std::min(one_past_last_command, command_count);
        if (begin >= end)
            return;

        BindingState& bs = *replay_state.binding_state;
        const PreBarrierGroupSelection pre_bsel = selectPreBarrierGroups(compiled_graph, frame_ctx);

        auto tryCallKernelFn = [&]() -> bool {
            if (replay_state.current_pass == UINT32_MAX)
                return false;
            const auto& cpass = compiled_graph.compiled_passes[replay_state.current_pass];
            if (!cpass.pass || !cpass.pass->kernel_fn)
                return false;

            VkExtent2D ext{};
            VkViewport kfn_vp{};
            VkRect2D kfn_sc{};
            if (replay_state.current_pass < compiled_graph.render_pass_layout.pass_to_group.size())
            {
                const uint32_t gidx =
                    compiled_graph.render_pass_layout.pass_to_group[replay_state.current_pass];
                if (gidx < record_context.group_extents.size())
                {
                    ext = record_context.group_extents[gidx];
                    kfn_vp = {0.f, 0.f, static_cast<float>(ext.width),
                              static_cast<float>(ext.height), 0.f, 1.f};
                    kfn_sc = {{0, 0}, ext};
                }
            }

            VkShaderStageFlags pc_stages = 0;
            if (replay_state.current_bp == VK_PIPELINE_BIND_POINT_GRAPHICS
                && cpass.render.pipeline_template_handle.valid())
            {
                const auto& tmpl = pipeline_manager_.getTemplate(
                    cpass.render.pipeline_template_handle);
                for (const auto& range : tmpl.push_constant_ranges)
                    if (range.offset < 8u && range.offset + range.size > 0u)
                        pc_stages |= range.stageFlags;
            }

            PipelineVariantBindUser pvbu{
                .cmd = cmd,
                .bind_point = replay_state.current_bp,
                .binding_state = &bs};
            PassRecordContext ctx{
                .cmd                      = cmd,
                .cpass                    = cpass,
                .graph                    = compiled_graph,
                .frame                    = frame_ctx,
                .pipeline_manager         = pipeline_manager_,
                .gpu_resource_manager     = gpu_mgr,
                .pipeline_layout          = replay_state.current_layout,
                .view                     = frame_ctx.view,
                .extent                   = ext,
                .viewport                 = kfn_vp,
                .scissor                  = kfn_sc,
                .pc_stage_flags           = pc_stages,
                .per_frame_views_         = &record_context.per_frame_views,
                .per_frame_views_by_mip_  = &record_context.per_frame_views_by_mip,
                .physical_resources_      = record_context.physical_resources_ptr,
                .pipeline_bind_user       = &pvbu,
                .bind_pipeline_variant_fn = &bindPipelineVariantBridge,
            };
            cpass.pass->kernel_fn(ctx);
            return true;
        };

        for (uint32_t ci = begin; ci < end; ++ci)
        {
            const auto& entry = program.commands[ci];
            const std::byte* data = program.command_data.data() + entry.data_offset;

            switch (entry.type)
            {
            case ECmd::SetPassContext:
            {
#if !defined(NDEBUG)
                // Close the debug label for the previous pass (if any).
                if (replay_state.current_pass != UINT32_MAX && fn_end_debug_label_)
                    fn_end_debug_label_(cmd);
#endif
                std::memcpy(&replay_state.current_pass, data, sizeof(uint32_t));
                const auto& cpass = compiled_graph.compiled_passes[replay_state.current_pass];
                replay_state.current_bp = (cpass.pass->type == ERGPassType::COMPUTE
                                        || cpass.pass->type == ERGPassType::ASYNC_COMPUTE)
                    ? VK_PIPELINE_BIND_POINT_COMPUTE
                    : VK_PIPELINE_BIND_POINT_GRAPHICS;
#if !defined(NDEBUG)
                // Open a debug label for the new pass.
                if (fn_begin_debug_label_ && cpass.pass)
                {
                    VkDebugUtilsLabelEXT label{};
                    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
                    label.pLabelName = cpass.pass->name.c_str();
                    label.color[0]   = 0.2f; label.color[1] = 0.6f;
                    label.color[2]   = 1.0f; label.color[3] = 1.0f;
                    fn_begin_debug_label_(cmd, &label);
                }
#endif
                break;
            }

            case ECmd::PipelineBarrier:
            {
                if (!replay_barriers)
                    break;

                // phase field retained for wire-format stability; only PRE
                // barriers are emitted now (split acquire/release removed).
                struct { uint32_t pass_index; uint32_t phase; } bd;
                std::memcpy(&bd, data, sizeof(bd));
                const auto& cpass = compiled_graph.compiled_passes[bd.pass_index];

                emitPassBarriers(cmd, bd.pass_index, cpass,
                                 pre_bsel.groups, pre_bsel.group_index_by_pass,
                                 record_context, physical_resources, frame_ctx.frame_index);
                break;
            }

            case ECmd::BeginRendering:
            {
                if (!replay_render_scope)
                    break;

                // Prebuilt template: header + per-attachment view patches.
                struct ViewPatch { uint32_t resource_index; uint8_t is_depth; uint8_t color_slot; uint8_t _pad[2]; };
                struct Header {
                    uint32_t group_idx;
                    uint32_t color_count;
                    VkAttachmentLoadOp color_op;
                    VkAttachmentLoadOp depth_op;
                    VkFormat depth_format;
                    uint8_t  patch_count;
                    uint8_t  _pad[3];
                };

                Header hdr;
                std::memcpy(&hdr, data, sizeof(Header));
                const auto* patches = reinterpret_cast<const ViewPatch*>(data + sizeof(Header));

                // Fill constant attachment template fields.
                std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments> color_attaches{};
                for (uint32_t ci = 0; ci < hdr.color_count; ++ci)
                {
                    auto& ca = color_attaches[ci];
                    ca.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ca.loadOp      = hdr.color_op;
                    ca.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
                    ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                }

                VkRenderingAttachmentInfo depth_attach{};
                depth_attach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth_attach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth_attach.loadOp      = hdr.depth_op;
                depth_attach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
                depth_attach.clearValue.depthStencil = {1.0f, 0};

                // Patch only the imageView fields from the compact patch table.
                for (uint8_t pi = 0; pi < hdr.patch_count; ++pi)
                {
                    const auto& vp = patches[pi];
                    VkImageView view = VK_NULL_HANDLE;
                    if (vp.resource_index < record_context.per_frame_views.size())
                    {
                        const auto& fv = record_context.per_frame_views[vp.resource_index];
                        if (frame_ctx.frame_index < fv.size())
                            view = fv[frame_ctx.frame_index];
                    }
                    if (view == VK_NULL_HANDLE) continue;
                    if (vp.is_depth)
                        depth_attach.imageView = view;
                    else if (vp.color_slot < hdr.color_count)
                        color_attaches[vp.color_slot].imageView = view;
                }

                VkRenderingInfo ri{};
                ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.renderArea.extent    = record_context.group_extents[hdr.group_idx];
                ri.layerCount           = record_context.group_layer_counts[hdr.group_idx];
                ri.colorAttachmentCount = hdr.color_count;
                ri.pColorAttachments    = hdr.color_count > 0 ? color_attaches.data() : nullptr;
                ri.pDepthAttachment     =
                    (hdr.depth_format != VK_FORMAT_UNDEFINED
                        && depth_attach.imageView != VK_NULL_HANDLE)
                    ? &depth_attach : nullptr;

                vkCmdBeginRendering(cmd, &ri);
                break;
            }

            case ECmd::EndRendering:
            {
                if (!replay_render_scope)
                    break;
                vkCmdEndRendering(cmd);
                break;
            }

            case ECmd::BindPipeline:
            {
                struct { VkPipeline pipeline; VkPipelineLayout layout; } bp;
                std::memcpy(&bp, data, sizeof(bp));
                vkCmdBindPipeline(cmd, replay_state.current_bp, bp.pipeline);
                if (bp.layout != VK_NULL_HANDLE)
                    replay_state.current_layout = bp.layout;
                bs.last_pipeline_layout = replay_state.current_layout;
                if (replay_state.current_bp == VK_PIPELINE_BIND_POINT_GRAPHICS)
                    bs.last_graphics_pipeline = bp.pipeline;
                else
                    bs.last_compute_pipeline = bp.pipeline;
                break;
            }

            case ECmd::BindDescriptorSets:
            {
                struct { uint32_t slot; VkDescriptorSet set; } bd;
                std::memcpy(&bd, data, sizeof(bd));

                VkDescriptorSet ds = bd.set;
                if (ds == VK_NULL_HANDLE && replay_state.current_pass != UINT32_MAX)
                {
                    const auto& cpass = compiled_graph.compiled_passes[replay_state.current_pass];
                    for (const auto& recipe : cpass.render.ds_bind_recipe)
                    {
                        if (recipe.slot == bd.slot && recipe.resolve)
                        {
                            ds = recipe.resolve(
                                recipe,
                                frame_ctx.frame_index,
                                frame_ctx.scene_ds,
                                &record_context.transient_descriptor_sets);
                            break;
                        }
                    }
                }
                if (ds == VK_NULL_HANDLE || replay_state.current_layout == VK_NULL_HANDLE)
                    break;

                vkCmdBindDescriptorSets(cmd, replay_state.current_bp, replay_state.current_layout,
                                        bd.slot, 1, &ds, 0, nullptr);
                break;
            }

            case ECmd::PushConstants:
            {
                uint32_t pass_index;
                std::memcpy(&pass_index, data, sizeof(uint32_t));
                const auto& cpass = compiled_graph.compiled_passes[pass_index];

                VkShaderStageFlags stages = 0;
                if (cpass.render.pipeline_template_handle.valid())
                {
                    const auto& tmpl = pipeline_manager_.getTemplate(cpass.render.pipeline_template_handle);
                    for (const auto& range : tmpl.push_constant_ranges)
                        if (range.offset < 8u && range.offset + range.size > 0u)
                            stages |= range.stageFlags;
                }
                if (stages != 0 && replay_state.current_layout != VK_NULL_HANDLE)
                {
                    struct { uint32_t scene_index; uint32_t view_index; } pc{
                        frame_ctx.scene_index, frame_ctx.view_index};
                    vkCmdPushConstants(cmd, replay_state.current_layout, stages, 0, sizeof(pc), &pc);
                }
                break;
            }

            case ECmd::SetViewport:
            {
                uint32_t pass_index;
                std::memcpy(&pass_index, data, sizeof(uint32_t));
                const uint32_t gidx = compiled_graph.render_pass_layout.pass_to_group[pass_index];
                const auto ext = record_context.group_extents[gidx];
                VkViewport vp{0.f, 0.f,
                              static_cast<float>(ext.width),
                              static_cast<float>(ext.height),
                              0.f, 1.f};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                break;
            }

            case ECmd::SetScissor:
            {
                uint32_t pass_index;
                std::memcpy(&pass_index, data, sizeof(uint32_t));
                const uint32_t gidx = compiled_graph.render_pass_layout.pass_to_group[pass_index];
                const auto ext = record_context.group_extents[gidx];
                VkRect2D sc{{0, 0}, ext};
                vkCmdSetScissor(cmd, 0, 1, &sc);
                break;
            }

            case ECmd::Dispatch:
            {
                if (tryCallKernelFn())
                    break;

                struct { uint32_t x, y, z; } d;
                std::memcpy(&d, data, sizeof(d));
                for (const auto& patch : program.patches)
                {
                    if (patch.command_index != ci)
                        continue;
                    if (patch.source == ExecutionProgram::DynamicPatch::ESource::KernelPatch)
                    {
                        const KernelTypeId kid = static_cast<KernelTypeId>(patch.source_param >> 8);
                        const uint16_t sub_source = patch.source_param & 0xFF;
                        const auto* desc = KernelRegistry::instance().find(kid);
                        if (desc && desc->resolve_patch)
                            d.x = desc->resolve_patch(sub_source, frame_ctx);
                    }
                }

                if (d.x > 0)
                    vkCmdDispatch(cmd, d.x, d.y, d.z);
                break;
            }

            case ECmd::DrawDirect:
            {
                if (tryCallKernelFn())
                    break;

                struct { uint32_t vtx_count, inst_count, first_vtx, first_inst; } d;
                std::memcpy(&d, data, sizeof(d));
                if (d.vtx_count > 0 && d.inst_count > 0)
                    vkCmdDraw(cmd, d.vtx_count, d.inst_count, d.first_vtx, d.first_inst);
                break;
            }

            case ECmd::DrawIndexedIndirectCount:
            {
                if (replay_state.skip_next_draw)
                {
                    // Skip ALL draws of an inactive shadow bias-group lane, not
                    // just the first. Do NOT clear the flag here — the next
                    // kDrawLaneSetup re-arms it (false for an active lane, true for
                    // the next inactive one; ShadowKernels.cpp:350/354). Clearing
                    // it after the first skipped draw let the lane's remaining
                    // view_mdc_count-1 draws run with never-set dynamic viewport/
                    // scissor/depth-bias (a VUID violation for lane 0). (C-7)
                    break;
                }

                struct {
                    uint32_t     indirect_resource_idx;
                    uint32_t     count_resource_idx;
                    VkDeviceSize indirect_offset;
                    VkDeviceSize count_offset;
                    uint32_t     max_draws;
                    uint32_t     stride;
                } d;
                std::memcpy(&d, data, sizeof(d));

                VkBuffer indirect_buf = VK_NULL_HANDLE;
                VkBuffer count_buf = VK_NULL_HANDLE;
                if (auto* pr = physical_resources.tryGet(d.indirect_resource_idx))
                    indirect_buf = reinterpret_cast<VkBuffer>(pr->getHandle(frame_ctx.frame_index));
                if (auto* pr = physical_resources.tryGet(d.count_resource_idx))
                    count_buf = reinterpret_cast<VkBuffer>(pr->getHandle(frame_ctx.frame_index));

                if (indirect_buf != VK_NULL_HANDLE && count_buf != VK_NULL_HANDLE)
                {
                    vkCmdDrawIndexedIndirectCount(
                        cmd,
                        indirect_buf,
                        d.indirect_offset,
                        count_buf,
                        d.count_offset,
                        d.max_draws,
                        d.stride);
                }
                break;
            }

            case ECmd::BindMeshBuffers:
            {
                auto* mesh = gpu_mgr ? gpu_mgr->find<MeshResources>() : nullptr;
                if (mesh)
                {
                    VkBuffer vbo = mesh->vertexBuffer();
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
                    vkCmdBindIndexBuffer(cmd, mesh->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                }
                break;
            }

            case ECmd::FillBuffer:
            {
                struct {
                    uint32_t     resource_idx;
                    VkDeviceSize offset;
                    VkDeviceSize size;
                    uint32_t     fill_value;
                } d;
                std::memcpy(&d, data, sizeof(d));

                VkBuffer buf = VK_NULL_HANDLE;
                if (auto* pr = physical_resources.tryGet(d.resource_idx))
                    buf = reinterpret_cast<VkBuffer>(pr->getHandle(frame_ctx.frame_index));
                if (buf != VK_NULL_HANDLE && d.size > 0)
                {
                    vkCmdFillBuffer(cmd, buf, d.offset, d.size, d.fill_value);

                    // Coalesce transfer->compute synchronization for consecutive fills.
                    const bool next_is_fill = (ci + 1u < end)
                                           && (program.commands[ci + 1u].type == ECmd::FillBuffer);
                    if (!next_is_fill)
                    {
                        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                              | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        dep.memoryBarrierCount = 1;
                        dep.pMemoryBarriers    = &barrier;
                        vkCmdPipelineBarrier2(cmd, &dep);
                    }
                }
                break;
            }

            case ECmd::ClearCounters:
            {
                struct {
                    uint32_t resource_indices[ClearCountersKernelConfig::kMaxBuffers];
                    uint32_t element_counts[ClearCountersKernelConfig::kMaxBuffers];
                    uint32_t buffer_count;
                    uint32_t dispatch_x;
                } d;
                std::memcpy(&d, data, sizeof(d));

                if (d.buffer_count == 0 || d.dispatch_x == 0
                    || replay_state.current_layout == VK_NULL_HANDLE)
                {
                    break;
                }

                const uint32_t clamped_count = std::min(
                    d.buffer_count,
                    ClearCountersKernelConfig::kMaxBuffers
                );
                bool all_buffers_valid = true;
                uint32_t max_elements = 0;
                for (uint32_t bi = 0; bi < clamped_count; ++bi)
                {
                    auto* pr = physical_resources.tryGet(d.resource_indices[bi]);
                    if (!pr)
                    {
                        all_buffers_valid = false;
                        break;
                    }

                    VkBuffer buf = reinterpret_cast<VkBuffer>(pr->getHandle(frame_ctx.frame_index));
                    if (buf == VK_NULL_HANDLE)
                    {
                        all_buffers_valid = false;
                        break;
                    }

                    max_elements = std::max(max_elements, d.element_counts[bi]);
                }

                if (!all_buffers_valid || max_elements == 0)
                    break;

                struct {
                    uint32_t element_count;
                    uint32_t buffer_count;
                    uint32_t element_counts[ClearCountersKernelConfig::kMaxBuffers];
                } pc{};
                pc.element_count = max_elements;
                pc.buffer_count = clamped_count;
                std::memcpy(pc.element_counts, d.element_counts, sizeof(pc.element_counts));

                vkCmdPushConstants(cmd,
                                   replay_state.current_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT,
                                   0,
                                   sizeof(pc),
                                   &pc);
                vkCmdDispatch(cmd, d.dispatch_x, 1u, 1u);
                break;
            }

            case ECmd::UploadViewFrustum:
            {
                uint32_t resource_idx;
                std::memcpy(&resource_idx, data, sizeof(uint32_t));

                if (frame_ctx.view != nullptr)
                if (auto* pr = physical_resources.tryGet(resource_idx))
                {
                    VkBuffer buf = reinterpret_cast<VkBuffer>(pr->getHandle(frame_ctx.frame_index));
                    if (buf != VK_NULL_HANDLE)
                    {
                        constexpr VkDeviceSize kFrustumSize = 6 * 16;
                        vkCmdUpdateBuffer(cmd, buf, 0, kFrustumSize, frame_ctx.view->frustum_staging.data());

                        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        dep.memoryBarrierCount = 1;
                        dep.pMemoryBarriers    = &barrier;
                        vkCmdPipelineBarrier2(cmd, &dep);
                    }
                }
                break;
            }

            case ECmd::CullPushConstants:
            {
                // SSOT struct shared with the view/shadow fills (GpuDrivenMeshConsts.hpp).
                // This is the byte-for-byte scratch the recorder pushes; slot_count and
                // the world-partition active-mask address are patched here from per-frame
                // ext data (both rotate per frame, so they can't be baked at emit time).
                MeshCullPushConstants pc{};
                std::memcpy(&pc, data,
                            std::min(sizeof(pc), static_cast<size_t>(entry.data_size)));
                {
                    const auto isl = meshInstanceExtSlot();
                    if (isl != kInvalidExtSlot && frame_ctx.ext_data[isl])
                    {
                        const auto* ext = static_cast<const MeshInstanceExtData*>(
                            frame_ctx.ext_data[isl]);
                        pc.slot_count       = ext->slot_count;
                        pc.active_mask_addr = ext->active_mask_addr;
                    }
                }

                if (pc.slot_count > 0 && replay_state.current_layout != VK_NULL_HANDLE)
                {
                    vkCmdPushConstants(cmd, replay_state.current_layout,
                                       VK_SHADER_STAGE_COMPUTE_BIT,
                                       0, sizeof(pc), &pc);
                }
                break;
            }

            case ECmd::InvokeKernelFn:
            {
                tryCallKernelFn();
                break;
            }

            case ECmd::CopyBuffer:
            {
                if (tryCallKernelFn())
                    break;
                break;
            }

            case ECmd::KernelCommand:
            {
                // Decode header: [KernelTypeId(1) | sub_cmd(1) | payload_size(2)]
                if (entry.data_size < 4)
                    break;
                struct Header { KernelTypeId kid; uint8_t sub; uint16_t psize; };
                Header hdr;
                std::memcpy(&hdr, data, sizeof(hdr));

                const auto* desc = KernelRegistry::instance().find(hdr.kid);
                if (desc && desc->replay)
                {
                    const void* payload = (entry.data_size > sizeof(hdr))
                        ? (static_cast<const std::byte*>(static_cast<const void*>(data)) + sizeof(hdr))
                        : nullptr;
                    KernelReplayContext kern_ctx{
                        cmd, frame_ctx, physical_resources,
                        replay_state.current_layout,
                        replay_state.skip_next_draw
                    };
                    desc->replay(hdr.sub, payload, hdr.psize, kern_ctx);
                }
                break;
            }
            }
        }
#if !defined(NDEBUG)
        // Close the debug label for the last pass (if the range emitted any).
        if (replay_state.current_pass != UINT32_MAX && fn_end_debug_label_)
            fn_end_debug_label_(cmd);
#endif
    }

    // recordPassContent — reusable helper for both primary and secondary CB paths
    // ================================
    void RGVulkanRecorder::recordPassContent(
        VkCommandBuffer cmd,
        const RGCompiledPass& cpass,
        const RGCompiledGraph& compiled_graph,
        RGRecordContext& record_context,
        const RGFrameContext& frame_ctx,
        ResourceRegistryBase* gpu_mgr,
        VkPipelineBindPoint bind_point,
        VkExtent2D extent)
    {
        const bool has_callback = cpass.pass && (cpass.pass->recorder || cpass.pass->kernel_fn);

        // Recorder fallback only executes callback-backed passes.
        if (!cpass.pass || !has_callback)
            return;

#if !defined(NDEBUG)
        // ---- VK_EXT_debug_utils: label this pass in the command buffer ------
        const char* pass_name = cpass.pass->name.c_str();
        struct LabelScope
        {
            VkCommandBuffer                cmd;
            PFN_vkCmdEndDebugUtilsLabelEXT end_fn;
            ~LabelScope() { if (end_fn) end_fn(cmd); }
        } label_scope{ cmd, fn_end_debug_label_ };
        if (fn_begin_debug_label_)
        {
            VkDebugUtilsLabelEXT label{};
            label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = pass_name;
            label.color[0]   = 1.f;  label.color[1] = 0.8f;
            label.color[2]   = 0.f;  label.color[3] = 1.f;
            fn_begin_debug_label_(cmd, &label);
        }
#endif

        // ------------------------------------------------------------------
        // Step 1: Bind pass-level pipeline (if declared and marked for binding)
        // ------------------------------------------------------------------
        const VkPipeline       pass_pipeline = cpass.render.pipeline;
        const VkPipelineLayout pass_layout   = cpass.render.pipeline_layout;
        BindingState& bs = record_context.binding_states[frame_ctx.frame_index];

        if (pass_pipeline != VK_NULL_HANDLE && cpass.render.bind_pipeline)
        {
#if !defined(NDEBUG)
            // Debug name the pipeline
            if (fn_object_name_
                && debug_named_pipelines_.find(pass_pipeline) == debug_named_pipelines_.end())
            {
                VkDebugUtilsObjectNameInfoEXT ni{};
                ni.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
                ni.objectType   = VK_OBJECT_TYPE_PIPELINE;
                ni.objectHandle = reinterpret_cast<uint64_t>(pass_pipeline);
                ni.pObjectName  = pass_name;
                fn_object_name_(context_.logicalDevice(), &ni);
                debug_named_pipelines_.insert(pass_pipeline);
            }
#endif
            vkCmdBindPipeline(cmd, bind_point, pass_pipeline);

            // Track for per-slot DS layout-break detection
            if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
                bs.last_graphics_pipeline = pass_pipeline;
            else
                bs.last_compute_pipeline  = pass_pipeline;
            bs.last_pipeline_layout = pass_layout;
        }

        // ------------------------------------------------------------------
        // Step 2: Bind descriptor sets from compile-time binding plan.
        // The compiler pre-computes bind_ds_mask which incorporates both
        // layout incompatibility and DS handle identity changes.
        // ------------------------------------------------------------------
        // Compute stageFlags for the shared [0,8) push constant from the template's
        // reflected PC ranges.  Using the wrong stageFlags triggers
        // VUID-vkCmdPushConstants-offset-01795 / 01796.
        VkShaderStageFlags shared_pc_stages = 0;
        if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS &&
            cpass.render.pipeline_template_handle.valid())
        {
            const auto& tmpl = pipeline_manager_.getTemplate(
                cpass.render.pipeline_template_handle);
            for (const auto& range : tmpl.push_constant_ranges)
                if (range.offset < 8u && range.offset + range.size > 0u)
                    shared_pc_stages |= range.stageFlags;
        }

        if (!cpass.render.ds_bind_recipe.empty() && pass_layout != VK_NULL_HANDLE)
        {
            const uint32_t bind_mask = cpass.render.bind_ds_mask;
            const auto* transient_sets = &record_context.transient_descriptor_sets;
            for (const DSBindRecipe& recipe : cpass.render.ds_bind_recipe)
            {
                const uint32_t slot = recipe.slot;
                if (!shouldBindDescriptorSlot(bind_mask, slot)) continue;

                VkDescriptorSet ds = VK_NULL_HANDLE;
                if (recipe.resolve != nullptr)
                {
                    ds = recipe.resolve(
                        recipe,
                        frame_ctx.frame_index,
                        frame_ctx.scene_ds,
                        transient_sets);
                }
                if (ds == VK_NULL_HANDLE) continue;

                vkCmdBindDescriptorSets(cmd, bind_point, pass_layout, slot, 1, &ds, 0, nullptr);
            }

            // Push scene_index + view_index as push constants (offset=0, 8 bytes)
            // for all graphics passes so shaders can index the SoA buffers.
            if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && shared_pc_stages != 0)
            {
                struct { uint32_t scene_index; uint32_t view_index; } pc{
                    frame_ctx.scene_index, frame_ctx.view_index
                };
                vkCmdPushConstants(cmd, pass_layout, shared_pc_stages, 0, sizeof(pc), &pc);
            }
        }

        // ------------------------------------------------------------------
        // Step 3: Auto-set viewport / scissor for graphics passes
        // ------------------------------------------------------------------
        const VkViewport auto_viewport{
            0.0f, 0.0f,
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
            0.0f, 1.0f};
        const VkRect2D auto_scissor{{0, 0}, extent};

        if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS
            && cpass.pass && !cpass.pass->manual_viewport)
        {
            vkCmdSetViewport(cmd, 0, 1, &auto_viewport);
            vkCmdSetScissor (cmd, 0, 1, &auto_scissor);
        }

        // ------------------------------------------------------------------
        // Step 4: Invoke feature recording lambda
        // ------------------------------------------------------------------
        PipelineVariantBindUser pipeline_bind_user{
            .cmd = cmd,
            .bind_point = bind_point,
            .binding_state = &bs,
        };
        PassRecordContext rec_ctx{
            .cmd                  = cmd,
            .cpass                = cpass,
            .graph                = compiled_graph,
            .frame                = frame_ctx,
            .pipeline_manager     = pipeline_manager_,
            .gpu_resource_manager = gpu_mgr,
            .pipeline_layout      = pass_layout,
            .view                 = frame_ctx.view,
            .extent               = extent,
            .viewport             = auto_viewport,
            .scissor              = auto_scissor,
            .pc_stage_flags       = shared_pc_stages,
            .per_frame_views_     = &record_context.per_frame_views,
            .per_frame_views_by_mip_ = &record_context.per_frame_views_by_mip,
            .physical_resources_  = record_context.physical_resources_ptr,
            .pipeline_bind_user   = &pipeline_bind_user,
            .bind_pipeline_variant_fn = &bindPipelineVariantBridge,
        };
        const auto& callback = cpass.pass->recorder ? cpass.pass->recorder : cpass.pass->kernel_fn;
        callback(rec_ctx);
    }

} // namespace lux::render
