#pragma once
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>
#include <type_traits>
#include <utility>
#include <vector>
#if !defined(NDEBUG)
#include <unordered_set>
#endif

namespace lux::render
{
    class ResourceContext;
    class PipelineManager;
    class ResourceRegistryBase;

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
            ResourceRegistryBase* gpu_mgr = nullptr
        ) override;

        // Provide a helper interface for manual destruction by the upper layer when needed
        void freeRecordContext(RGRecordContext& record_context);

    private:
        template<typename CleanupFn>
        class ScopeGuard
        {
        public:
            explicit ScopeGuard(CleanupFn fn) noexcept(std::is_nothrow_move_constructible_v<CleanupFn>)
                : fn_(std::move(fn))
            {
            }

            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;

            ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<CleanupFn>)
                : fn_(std::move(other.fn_)), active_(other.active_)
            {
                other.active_ = false;
            }

            ScopeGuard& operator=(ScopeGuard&&) = delete;

            ~ScopeGuard() noexcept
            {
                if (active_)
                    fn_();
            }

            void dismiss() noexcept { active_ = false; }

        private:
            CleanupFn fn_;
            bool active_{ true };
        };

        template<typename CleanupFn>
        static ScopeGuard<std::decay_t<CleanupFn>> makeScopeGuard(CleanupFn&& fn)
        {
            return ScopeGuard<std::decay_t<CleanupFn>>(std::forward<CleanupFn>(fn));
        }

        struct ExecutionReplayState;

        /// Eagerly create all per-frame attachment VkImageViews so the hot record() path is a pure lookup.
        std::optional<std::error_code> preCreateImageViews(RGRecordContext& record_context, const RGCompiledGraph& graph, const RGPhysicalResourceTable& physical_resources, uint32_t frames_in_flight);
        std::optional<std::error_code> computeGroupExtents(RGRecordContext& record_context, const RGCompiledGraph& graph, VkExtent2D extent);

        void destroyImageViews(RGRecordContext& record_context);

        VkImageView resolveImageView(const RGCompiledGraph& graph, const RGPhysicalResourceTable& physical_resources, RGRecordContext& record_context, RGResourceHandle handle, uint32_t frame_index, lux::common::ETextureRole role) const;

        void recordPassContent(
            VkCommandBuffer cmd,
            const RGCompiledPass& cpass,
            const RGCompiledGraph& compiled_graph,
            RGRecordContext& record_context,
            const RGFrameContext& frame_ctx,
            ResourceRegistryBase* gpu_mgr,
            VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
            VkExtent2D extent = {});

        /// Phase 2 fast-path executor: linearly replays the pre-compiled
        /// ExecutionProgram, bypassing recorder-lambda dispatch entirely.
        void executeFast(
            VkCommandBuffer cmd,
            const RGCompiledGraph& compiled_graph,
            RGRecordContext& record_context,
            const RGFrameContext& frame_ctx,
            const RGPhysicalResourceTable& physical_resources,
            ResourceRegistryBase* gpu_mgr);

        void replayExecutionRange(
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

        const ResourceContext&          context_;
        PipelineManager&                pipeline_manager_;
        bool                            use_dynamic_rendering_{ false };

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
