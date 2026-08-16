#pragma once
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/core/scope_exit.hpp>
#include <vulkan/vulkan.h>
#include <vector>
#if !defined(NDEBUG)
#include <array>
#include <unordered_set>
#endif

namespace lux::render
{
    class ResourceContext;
    class PipelineManager;
    class ResourceRegistry;

    class LUX_FUNCTION_PUBLIC RGVulkanRecorder : public RGRecorder
    {
    public:
        explicit RGVulkanRecorder(const ResourceContext& context, PipelineManager& pipeline_manager);

        Expected<RGRecordContext> allocateRecordContext(const RGCompiledGraph& compiled_graph, const RGPhysicalResourceTable& physical_resources, VkExtent2D extent, uint32_t frames_in_flight) override;

        bool deallocateRecordContext(RGRecordContext& record_context) override;

        void refreshDynamicImportedResources(
            RGResourceState& resource_state,
            const RGCompiledGraph& compiled_graph) override;

        void record(
            RGResourceState& resource_state, 
            const RGCompiledGraph& compiled_graph, 
            const RGFrameContext& context, 
            VkCommandBuffer target_cmd, 
            ResourceRegistry* gpu_mgr = nullptr
        ) override;

        // Provide a helper interface for manual destruction by the upper layer when needed
        void freeRecordContext(RGRecordContext& record_context);

    private:
        struct ExecutionReplayState;

        /// Eagerly create all per-frame attachment VkImageViews so the hot record() path is a pure lookup.
        std::optional<RenderError> preCreateImageViews(RGRecordContext& record_context, const RGCompiledGraph& graph, const RGPhysicalResourceTable& physical_resources, uint32_t frames_in_flight);
        std::optional<RenderError> computeGroupExtents(RGRecordContext& record_context, const RGCompiledGraph& graph, VkExtent2D extent);

        void destroyImageViews(RGRecordContext& record_context);


        void recordPassContent(
            VkCommandBuffer cmd,
            const RGCompiledPass& cpass,
            const RGCompiledGraph& compiled_graph,
            RGRecordContext& record_context,
            const RGFrameContext& frame_ctx,
            ResourceRegistry* gpu_mgr,
            VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
            VkExtent2D extent = {});

        /// 字节码快路执行器:线性重放预编译的
        /// ExecutionProgram, bypassing recorder-lambda dispatch entirely.
        void executeFast(
            VkCommandBuffer cmd,
            const RGCompiledGraph& compiled_graph,
            RGRecordContext& record_context,
            const RGFrameContext& frame_ctx,
            const RGPhysicalResourceTable& physical_resources,
            ResourceRegistry* gpu_mgr);

        void replayExecutionRange(
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
            bool replay_render_scope);

        /// Build and append multi-queue timeline-semaphore submission structs.
        void buildMultiQueueSubmit(
            RGRecordContext& ctx,
            const RGCompiledGraph& graph,
            VkCommandBuffer cmd,
            VkCommandBuffer compute_cmd,
            VkCommandBuffer transfer_cmd);

        /// Emit graph-export barriers (e.g. swapchain PRESENT transition).
        void submitFinalBarriers(
            VkCommandBuffer cmd,
            const RGCompiledGraph& graph,
            const RGPhysicalResourceTable& physical_resources,
            RGRecordContext& ctx,
            uint32_t frame_index);

        /// local-read 合并作用域的每-subpass 边界状态(remap 声明 + by-region
        /// 屏障)。成员而非文件级 static:入口指针按本 recorder 的设备解析。
        void applyLocalReadBoundaryState(VkCommandBuffer cmd,
                                         uint32_t color_count,
                                         const uint32_t* locations,
                                         const uint32_t* input_indices,
                                         uint32_t depth_input_index,
                                         bool emit_barrier);

        const ResourceContext&          context_;
        PipelineManager&                pipeline_manager_;
        bool                            use_dynamic_rendering_{ false };

        // ---- KHR_dynamic_rendering_local_read 入口 + 深度槽保活环 ----
        // 构造期按设备解析一次:函数级 static 会忽略 VkDevice 身份(多设备
        // 拿错入口),且两个指针分别赋值非原子,首帧并发录制可能读到 null
        // 静默丢掉整段 remap。深度槽环:某些验证层构建会保留
        // vkCmdSetRenderingInputAttachmentIndices 收到的指针到 draw 时解引用,
        // 栈地址会悬空;录制单线程,recorder 成员环即可。
        PFN_vkCmdSetRenderingAttachmentLocations    fn_set_rendering_locations_{ nullptr };
        PFN_vkCmdSetRenderingInputAttachmentIndices fn_set_rendering_inputs_{ nullptr };
        std::array<uint32_t, 64>        lr_depth_slots_{};
        uint32_t                        lr_depth_slot_next_{ 0 };

#if !defined(NDEBUG)
        // ---- VK_EXT_debug_utils: pass labels + pipeline object names ----
        PFN_vkCmdBeginDebugUtilsLabelEXT   fn_begin_debug_label_{ nullptr };
        PFN_vkCmdEndDebugUtilsLabelEXT     fn_end_debug_label_{ nullptr };
        PFN_vkSetDebugUtilsObjectNameEXT   fn_object_name_{ nullptr };
        /// Tracks which VkPipeline handles have already been named this session.
        std::unordered_set<VkPipeline>     debug_named_pipelines_;
#endif
    };
}
