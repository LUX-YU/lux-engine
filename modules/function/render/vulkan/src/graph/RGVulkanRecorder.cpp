#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp> // For ResourceContext
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGBarrierUtils.hpp>
#include <lux/engine/render/graph/KernelReplayContext.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/scene/View.hpp>   // frame_ctx.view->handle.index (per-view DS resolution)
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <array>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <unordered_map>

namespace lux::render
{
    // ---------- Local-read merged-scope boundary state ----------
    // Shared by the ExecutionProgram executor (LocalReadBoundary command) and
    // the serial dynamic-rendering path: sets this sub-pass's attachment
    // location / input-index remaps and, for non-first sub-passes, the
    // by-region intra-scope barrier. KHR entry points resolved lazily (the
    // promoted-core names may be absent from the static loader on 1.3 devices).
    void RGVulkanRecorder::applyLocalReadBoundaryState(VkCommandBuffer cmd,
                                                       uint32_t color_count,
                                                       const uint32_t* locations,
                                                       const uint32_t* input_indices,
                                                       uint32_t depth_input_index,
                                                       bool emit_barrier)
    {
        if (!fn_set_rendering_locations_ || !fn_set_rendering_inputs_)
            return;   // caps gate should prevent this; degrade to no-op

        if (emit_barrier)
        {
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            mb.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.dependencyFlags    = VK_DEPENDENCY_BY_REGION_BIT;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers    = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        VkRenderingAttachmentLocationInfo loc_info{};
        loc_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO;
        loc_info.colorAttachmentCount      = color_count;
        loc_info.pColorAttachmentLocations = locations;
        fn_set_rendering_locations_(cmd, &loc_info);

        // The depth index lives in a persistent ring: some validation-layer
        // builds keep the POINTER from vkCmdSetRenderingInputAttachmentIndices
        // and dereference it at draw time — a stack address would be dangling.
        uint32_t* depth_slot = nullptr;
        if (depth_input_index != VK_ATTACHMENT_UNUSED)
        {
            depth_slot  = &lr_depth_slots_[lr_depth_slot_next_++ % lr_depth_slots_.size()];
            *depth_slot = depth_input_index;
        }

        VkRenderingInputAttachmentIndexInfo input_info{};
        input_info.sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO;
        input_info.colorAttachmentCount         = color_count;
        input_info.pColorAttachmentInputIndices = input_indices;
        input_info.pDepthInputAttachmentIndex   = depth_slot;
        fn_set_rendering_inputs_(cmd, &input_info);
    }

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
            uint32_t frame_index,
            const std::vector<uint8_t>* src_is_final_state = nullptr)
        {
            out.clear(); // capacity already pre-reserved by allocateRecordContext (§1.1)

            const size_t count = std::min(src.size(), patch_resource_idx.size());
            for (size_t i = 0; i < count; ++i)
            {
                const uint32_t ri = patch_resource_idx[i];
                VkImage image = resolveImageHandle(
                    record_context,
                    physical_resources,
                    ri,
                    frame_index);
                if (image == VK_NULL_HANDLE)
                    continue;

                out.push_back(src[i]);
                out.back().image = image;

                // 运行时布局参数化:跨视图首触屏障的 oldLayout/src 同步域
                // 在编译期按当时的 final_layout 烙定;final_layout 已退出编译
                // 键,这里按当前录制 target 的槽位描述补丁(该图像上一次录制
                // 结束时被转到的正是本 target 的 final_layout——稳态不变量)。
                if (src_is_final_state && i < src_is_final_state->size() &&
                    (*src_is_final_state)[i] &&
                    record_context.target_layout && record_context.slot_resource_idx)
                {
                    for (size_t si = 0; si < kTargetSlotCount; ++si)
                    {
                        if (record_context.slot_resource_idx[si] != ri)
                            continue;
                        const auto s = static_cast<TargetSlot>(si);
                        if (record_context.target_layout->hasSlot(s))
                        {
                            const auto final_state = record_context.target_layout
                                ->slot(s).final_state;
                            const VkImageLayout fl =
                                toVkImageLayout(final_state);
                            if (fl != VK_IMAGE_LAYOUT_UNDEFINED)
                            {
                                auto& b = out.back();
                                b.oldLayout = fl;
                                const auto sync2 = finalSyncForState(final_state);
                                b.srcStageMask  = sync2.stage;
                                b.srcAccessMask = sync2.access;
                            }
                        }
                        break;
                    }
                }
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
                               record_context, physical_resources, frame_index,
                               pre_group ? &pre_group->image_src_is_final_state : nullptr);

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

        bool timestampRangeValid(
            const RGRecordContext& context,
            const RGCompiledGraph& graph,
            uint32_t frame_index) noexcept
        {
            return context.timestamp_pool != VK_NULL_HANDLE
                && context.timestamp_pass_capacity > 0
                && frame_index < context.timestamp_frame_ids.size()
                && graph.compiled_passes.size()
                    <= context.timestamp_pass_capacity
                && !graph.multi_queue_info.has_async_work;
        }

        void writePassTimestamp(
            VkCommandBuffer command_buffer,
            const RGRecordContext& context,
            const RGCompiledGraph& graph,
            uint32_t frame_index,
            uint32_t pass_index,
            bool begin) noexcept
        {
            if (!timestampRangeValid(context, graph, frame_index)
                || pass_index >= graph.compiled_passes.size())
            {
                return;
            }

            const uint32_t base = frame_index
                * context.timestamp_pass_capacity * 2u;
            vkCmdWriteTimestamp2(
                command_buffer,
                begin
                    ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                    : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                context.timestamp_pool,
                base + pass_index * 2u + (begin ? 0u : 1u)
            );
        }

        void updateImportedTransientDescriptors(
            VkDevice device,
            const RGCompiledGraph& graph,
            RGRecordContext& context,
            uint32_t frame_index)
        {
            const auto& descriptions =
                graph.original_graph.transient_descriptor_sets;
            for (std::size_t set_index = 0;
                 set_index < descriptions.size()
                    && set_index < context.transient_descriptor_sets.size();
                 ++set_index)
            {
                auto& per_frame =
                    context.transient_descriptor_sets[set_index];
                if (frame_index >= per_frame.size())
                    continue;

                std::vector<VkWriteDescriptorSet> writes;
                std::vector<VkDescriptorImageInfo> images;
                std::vector<VkDescriptorBufferInfo> buffers;
                writes.reserve(descriptions[set_index].writes.size());
                images.reserve(descriptions[set_index].writes.size());
                buffers.reserve(descriptions[set_index].writes.size());

                for (const auto& source : descriptions[set_index].writes)
                {
                    const auto resource_index = source.resource.index;
                    if (resource_index >= graph.original_graph.resources.size())
                        continue;
                    const auto& resource =
                        graph.original_graph.resources[resource_index];
                    if (!resource.import_info && !resource.import_buffer_info)
                        continue;

                    VkWriteDescriptorSet write{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
                    };
                    write.dstSet = per_frame[frame_index];
                    write.dstBinding = source.binding;
                    write.descriptorCount = 1u;
                    write.descriptorType = convertDescriptorType(
                        source.descriptor_type
                    );

                    switch (write.descriptorType)
                    {
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    {
                        VkImageView view = VK_NULL_HANDLE;
                        if (resource_index < context.per_frame_views.size()
                            && frame_index <
                                context.per_frame_views[resource_index].size())
                        {
                            view = context.per_frame_views[resource_index]
                                [frame_index];
                        }
                        if (view == VK_NULL_HANDLE)
                            continue;
                        images.push_back(VkDescriptorImageInfo{
                            source.sampler,
                            view,
                            convertImageLayout(source.image_layout),
                        });
                        write.pImageInfo = &images.back();
                        break;
                    }
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    {
                        VkBuffer buffer = VK_NULL_HANDLE;
                        if (resource_index < context.per_frame_buffers.size()
                            && frame_index <
                                context.per_frame_buffers[resource_index].size())
                        {
                            buffer = context.per_frame_buffers[resource_index]
                                [frame_index];
                        }
                        if (buffer == VK_NULL_HANDLE)
                            continue;
                        buffers.push_back(VkDescriptorBufferInfo{
                            buffer,
                            0u,
                            VK_WHOLE_SIZE,
                        });
                        write.pBufferInfo = &buffers.back();
                        break;
                    }
                    default:
                        continue;
                    }
                    writes.push_back(write);
                }

                if (!writes.empty())
                {
                    vkUpdateDescriptorSets(
                        device,
                        static_cast<uint32_t>(writes.size()),
                        writes.data(),
                        0u,
                        nullptr
                    );
                }
            }
        }
    } // namespace

    RGVulkanRecorder::RGVulkanRecorder(const ResourceContext& context, PipelineManager& pipeline_manager)
        : context_(context), pipeline_manager_(pipeline_manager),
          use_dynamic_rendering_(pipeline_manager.useDynamicRendering())
    {
        // KHR_dynamic_rendering_local_read 入口:按本设备解析,KHR 名优先
        // (1.3 设备的静态 loader 可能没有 promoted-core 名)。扩展未启用时
        // 两指针为 null,applyLocalReadBoundaryState 退化 no-op。
        const VkDevice dev = context_.logicalDevice();
        fn_set_rendering_locations_ = reinterpret_cast<PFN_vkCmdSetRenderingAttachmentLocations>(
            vkGetDeviceProcAddr(dev, "vkCmdSetRenderingAttachmentLocationsKHR"));
        if (!fn_set_rendering_locations_)
            fn_set_rendering_locations_ = reinterpret_cast<PFN_vkCmdSetRenderingAttachmentLocations>(
                vkGetDeviceProcAddr(dev, "vkCmdSetRenderingAttachmentLocations"));
        fn_set_rendering_inputs_ = reinterpret_cast<PFN_vkCmdSetRenderingInputAttachmentIndices>(
            vkGetDeviceProcAddr(dev, "vkCmdSetRenderingInputAttachmentIndicesKHR"));
        if (!fn_set_rendering_inputs_)
            fn_set_rendering_inputs_ = reinterpret_cast<PFN_vkCmdSetRenderingInputAttachmentIndices>(
                vkGetDeviceProcAddr(dev, "vkCmdSetRenderingInputAttachmentIndices"));

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
            return renderFailure<err::graph::CompiledGraphInvalid>();

        RGRecordContext record_context{};
        // Ensure partially-created Vulkan objects are released on any early return.
        auto cleanup_guard = lux::cxx::scope_exit([this, &record_context]() {
            freeRecordContext(record_context);
        });

        record_context.frames_in_flight = frames_in_flight;
        record_context.use_dynamic_rendering = use_dynamic_rendering_;
        // Initialize per-frame binding states
        record_context.binding_states.resize(frames_in_flight);

        // Timestamp support is optional. A missing timestamp-capable graphics
        // queue leaves an explicit unavailable snapshot and never changes graph
        // bring-up success. Query results are read only when a FIF slot's fence
        // has retired, so there is no GPU wait in this path.
        {
            const auto& device_context = context_.deviceContext();
            const auto& physical = device_context.physicalDevice();
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(
                static_cast<VkPhysicalDevice>(physical),
                &family_count,
                nullptr
            );
            std::vector<VkQueueFamilyProperties> families(family_count);
            if (family_count > 0)
            {
                vkGetPhysicalDeviceQueueFamilyProperties(
                    static_cast<VkPhysicalDevice>(physical),
                    &family_count,
                    families.data()
                );
            }

            const uint32_t family = device_context.graphicsQueueFamilyIndex();
            const auto pass_count = static_cast<uint32_t>(
                compiled_graph.compiled_passes.size()
            );
            const bool supported = family < families.size()
                && families[family].timestampValidBits > 0
                && pass_count > 0
                && frames_in_flight > 0
                && pass_count <= UINT32_MAX / (2u * frames_in_flight);

            record_context.latest_gpu_timing =
                std::make_shared<const RGGpuTimingSnapshot>();
            if (supported)
            {
                VkQueryPoolCreateInfo query_info{
                    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO
                };
                query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
                query_info.queryCount = pass_count * 2u * frames_in_flight;
                if (vkCreateQueryPool(
                        context_.logicalDevice(),
                        &query_info,
                        context_.instanceContext().allocator(),
                        &record_context.timestamp_pool
                    ) == VK_SUCCESS)
                {
                    record_context.timestamp_pass_capacity = pass_count;
                    record_context.timestamp_period_nanoseconds =
                        physical.properties().properties.limits.timestampPeriod;
                    record_context.timestamp_frame_ids.assign(
                        frames_in_flight,
                        0u
                    );
                    record_context.timestamp_result_scratch.resize(
                        static_cast<std::size_t>(pass_count) * 4u
                    );
                }
            }
        }

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
                return renderFailure<err::device::VulkanObjectCreationFailed>();

            // Async compute command pool + per-frame command buffers
            if (!compiled_graph.multi_queue_info.compute_order.empty())
            {
                VkCommandPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_ci.queueFamilyIndex = dev_ctx.asyncComputeQueueFamilyIndex();

                if (vkCreateCommandPool(device, &pool_ci, context_.instanceContext().allocator(),
                                        &record_context.compute_cmd_pool) != VK_SUCCESS)
                    return renderFailure<err::device::VulkanObjectCreationFailed>();

                record_context.compute_cmd_bufs.resize(frames_in_flight, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo alloc_ci{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                alloc_ci.commandPool        = record_context.compute_cmd_pool;
                alloc_ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_ci.commandBufferCount = frames_in_flight;

                if (vkAllocateCommandBuffers(device, &alloc_ci, record_context.compute_cmd_bufs.data()) != VK_SUCCESS)
                    return renderFailure<err::device::VulkanObjectCreationFailed>();
            }

            // Async transfer command pool + per-frame command buffers
            if (!compiled_graph.multi_queue_info.transfer_order.empty())
            {
                VkCommandPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_ci.queueFamilyIndex = dev_ctx.transferQueueFamilyIndex();

                if (vkCreateCommandPool(device, &pool_ci, context_.instanceContext().allocator(),
                                        &record_context.transfer_cmd_pool) != VK_SUCCESS)
                    return renderFailure<err::device::VulkanObjectCreationFailed>();

                record_context.transfer_cmd_bufs.resize(frames_in_flight, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo alloc_ci{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                alloc_ci.commandPool        = record_context.transfer_cmd_pool;
                alloc_ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_ci.commandBufferCount = frames_in_flight;

                if (vkAllocateCommandBuffers(device, &alloc_ci, record_context.transfer_cmd_bufs.data()) != VK_SUCCESS)
                    return renderFailure<err::device::VulkanObjectCreationFailed>();
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
                return renderFailure<err::device::VulkanObjectCreationFailed>();

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
                    return renderFailure<err::device::VulkanObjectCreationFailed>();

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
                            // Imported target-slot images are injected only at
                            // record time. Do not publish a null image view to
                            // Vulkan here; updateImportedTransientDescriptors()
                            // fills this binding after that injection.
                            if (dview == VK_NULL_HANDLE)
                                continue;
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
                            if (bi.buffer == VK_NULL_HANDLE)
                                continue;
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

        cleanup_guard.release();
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

        if (record_context.timestamp_pool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(
                device,
                record_context.timestamp_pool,
                alloc
            );
            record_context.timestamp_pool = VK_NULL_HANDLE;
        }
        record_context.timestamp_frame_ids.clear();
        record_context.timestamp_result_scratch.clear();
        record_context.latest_gpu_timing.reset();
        record_context.gpu_timing_history.clear();

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
        const RGRecordContext&   record_ctx,
        const RGCompiledPass&    sub_cpass,
        const RGRenderPassGroup& group,
        const RGPassInRenderPass* pass_info,
        uint32_t                 frame_index,
        VkAttachmentLoadOp       color_op,
        VkAttachmentLoadOp       depth_op,
        std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments>& out_color,
        VkRenderingAttachmentInfo& out_depth,
        std::vector<uint32_t>*   pending_clear = nullptr)
    {
        // 消费"被跳过首写者"登记:该资源本帧的 CLEAR 义务转移到本 pass。
        auto take_pending_clear = [&](uint32_t res_idx) -> bool {
            if (!pending_clear) return false;
            for (auto it = pending_clear->begin(); it != pending_clear->end(); ++it)
                if (*it == res_idx) { pending_clear->erase(it); return true; }
            return false;
        };
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
        out_depth.loadOp      = (pass_info != nullptr) ? pass_info->pass_depth_load_op : depth_op;
        out_depth.storeOp     = (pass_info != nullptr) ? pass_info->pass_depth_store_op
                                                       : VK_ATTACHMENT_STORE_OP_STORE;
        out_depth.clearValue.depthStencil = {1.0f, 0};

        if (!sub_cpass.pass) return;
        uint32_t color_idx  = 0;
        uint32_t color_decl = 0; // 本 pass 的 COLOR_ATTACHMENT 声明序,对齐 pass_color_load_ops
        for (const auto& tex_ref : sub_cpass.pass->textures)
        {
            VkImageView view = VK_NULL_HANDLE;
            if (tex_ref.resource.index < record_ctx.per_frame_views.size())
            {
                const auto& fv = record_ctx.per_frame_views[tex_ref.resource.index];
                if (frame_index < fv.size())
                    view = fv[frame_index];
            }

            if (tex_ref.role == lux::common::ETextureRole::COLOR_ATTACHMENT)
            {
                const uint32_t decl = color_decl++;
                if (view == VK_NULL_HANDLE) continue;
                if (color_idx < group.key.color_count)
                {
                    if (pass_info && decl < pass_info->pass_color_load_ops.size())
                        out_color[color_idx].loadOp = pass_info->pass_color_load_ops[decl];
                    if (out_color[color_idx].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                        take_pending_clear(tex_ref.resource.index))
                        out_color[color_idx].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    if (pass_info && decl < pass_info->pass_color_store_ops.size())
                        out_color[color_idx].storeOp = pass_info->pass_color_store_ops[decl];
                    out_color[color_idx++].imageView = view;
                }
            }
            else if (tex_ref.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT &&
                     view != VK_NULL_HANDLE)
            {
                if (out_depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                    take_pending_clear(tex_ref.resource.index))
                    out_depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                out_depth.imageView = view;
            }
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
#include "RGVulkanRecorder.Record.inl"

    void RGVulkanRecorder::executeFast(
        VkCommandBuffer cmd,
        const RGCompiledGraph& compiled_graph,
        RGRecordContext& record_context,
        const RGFrameContext& frame_ctx,
        const RGPhysicalResourceTable& physical_resources,
        ResourceRegistry* gpu_mgr)
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
        ResourceRegistry* gpu_mgr,
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
                    if (range.offset < kViewPushPrefixSize && range.offset + range.size > 0u)
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
                if (replay_state.current_pass != UINT32_MAX)
                {
                    writePassTimestamp(
                        cmd,
                        record_context,
                        compiled_graph,
                        frame_ctx.frame_index,
                        replay_state.current_pass,
                        false
                    );
                }
#if !defined(NDEBUG)
                // Close the debug label for the previous pass (if any).
                if (replay_state.current_pass != UINT32_MAX && fn_end_debug_label_)
                    fn_end_debug_label_(cmd);
#endif
                std::memcpy(&replay_state.current_pass, data, sizeof(uint32_t));
                writePassTimestamp(
                    cmd,
                    record_context,
                    compiled_graph,
                    frame_ctx.frame_index,
                    replay_state.current_pass,
                    true
                );
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
                // wire structs shared with the emit side — RGCompiledGraph.hpp.
                using ViewPatch = ExecutionProgram::BeginRenderingViewPatch;
                using Header    = ExecutionProgram::BeginRenderingHeader;

                Header hdr;
                std::memcpy(&hdr, data, sizeof(Header));
                const auto* patches = reinterpret_cast<const ViewPatch*>(data + sizeof(Header));

                // Fill constant attachment template fields. Input-read slots of a
                // local-read merged group live in RENDERING_LOCAL_READ for the
                // whole scope (write + subpassLoad legal); untouched slots keep
                // the classic attachment layouts.
                std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments> color_attaches{};
                for (uint32_t ci = 0; ci < hdr.color_count; ++ci)
                {
                    auto& ca = color_attaches[ci];
                    ca.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    ca.imageLayout = ((hdr.lr_flags & 0x1) && ((hdr.lr_color_input_mask >> ci) & 0x1))
                        ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ
                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ca.loadOp      = (hdr.lr_flags & 0x1)
                        ? (((hdr.lr_color_clear_mask >> ci) & 0x1)
                               ? VK_ATTACHMENT_LOAD_OP_CLEAR
                               : VK_ATTACHMENT_LOAD_OP_LOAD)
                        : hdr.color_op;
                    ca.storeOp     = ((hdr.store_dontcare_mask >> ci) & 0x1)
                        ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                        : VK_ATTACHMENT_STORE_OP_STORE;
                    ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                }

                VkRenderingAttachmentInfo depth_attach{};
                depth_attach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth_attach.imageLayout = (hdr.lr_flags & 0x2)
                    ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ
                    : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth_attach.loadOp      = hdr.depth_op;
                depth_attach.storeOp     = (hdr.lr_flags & 0x4)
                    ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                    : VK_ATTACHMENT_STORE_OP_STORE;
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

            case ECmd::LocalReadBoundary:
            {
                // Line-B P2: sub-pass boundary inside a local-read merged scope.
                // Sets this sub-pass's attachment-location / input-index remaps
                // and (for sub-pass > 0) the by-region intra-scope barrier.
                if (!replay_render_scope)
                    break;

                // wire struct shared with the emit side (RGCompiledGraph.hpp).
                using Boundary = ExecutionProgram::LocalReadBoundaryPayload;
                Boundary b;
                std::memcpy(&b, data, sizeof(Boundary));

                applyLocalReadBoundaryState(cmd,
                                            b.color_count, b.locations,
                                            b.input_indices, b.depth_input_index,
                                            b.emit_barrier != 0);
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
                                &record_context.transient_descriptor_sets,
                                frame_ctx.view ? frame_ctx.view->handle.index : 0u);
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
                        if (range.offset < kViewPushPrefixSize && range.offset + range.size > 0u)
                            stages |= range.stageFlags;
                }
                if (stages != 0 && replay_state.current_layout != VK_NULL_HANDLE)
                {
                    ViewPushPrefix pc{frame_ctx.scene_index, frame_ctx.view_index};
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

            case ECmd::DispatchIndirect:
            {
                struct
                {
                    uint32_t resource_idx;
                    VkDeviceSize offset;
                } d{};
                std::memcpy(&d, data, sizeof(d));
                VkBuffer buffer = VK_NULL_HANDLE;
                if (auto* physical = physical_resources.tryGet(
                        d.resource_idx))
                {
                    buffer = reinterpret_cast<VkBuffer>(
                        physical->getHandle(frame_ctx.frame_index));
                }
                if (buffer != VK_NULL_HANDLE)
                    vkCmdDispatchIndirect(cmd, buffer, d.offset);
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
        if (replay_state.current_pass != UINT32_MAX)
        {
            writePassTimestamp(
                cmd,
                record_context,
                compiled_graph,
                frame_ctx.frame_index,
                replay_state.current_pass,
                false
            );
            replay_state.current_pass = UINT32_MAX;
        }
    }

    // recordPassContent — reusable helper for both primary and secondary CB paths
    // ================================
    void RGVulkanRecorder::recordPassContent(
        VkCommandBuffer cmd,
        const RGCompiledPass& cpass,
        const RGCompiledGraph& compiled_graph,
        RGRecordContext& record_context,
        const RGFrameContext& frame_ctx,
        ResourceRegistry* gpu_mgr,
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
                if (range.offset < kViewPushPrefixSize && range.offset + range.size > 0u)
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
                    // 录制是 per-view 的(FrameOrchestrator::runChain →
                    // Renderer::renderSingleView),而图的 template 是 per-scene 的。
                    // 持有 per-view 描述符集的资源靠这个 id 选对那一份。
                    const uint32_t view_id =
                        frame_ctx.view ? frame_ctx.view->handle.index : 0u;
                    ds = recipe.resolve(
                        recipe,
                        frame_ctx.frame_index,
                        frame_ctx.scene_ds,
                        transient_sets,
                        view_id);
                }
                if (ds == VK_NULL_HANDLE) continue;

                vkCmdBindDescriptorSets(cmd, bind_point, pass_layout, slot, 1, &ds, 0, nullptr);
            }

            // Push scene_index + view_index as push constants (offset=0, 8 bytes)
            // for all graphics passes so shaders can index the SoA buffers.
            if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && shared_pc_stages != 0)
            {
                ViewPushPrefix pc{frame_ctx.scene_index, frame_ctx.view_index};
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
