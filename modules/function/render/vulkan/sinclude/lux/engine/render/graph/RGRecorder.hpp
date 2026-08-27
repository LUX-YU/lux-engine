#pragma once
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/BindingState.hpp>
#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <vulkan/vulkan.h>
#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::render
{
    struct View;

    struct RGGpuPassTiming final
    {
        std::string name;
        double milliseconds{0.0};
    };

    /// Fence-retired timing sample for one rendered view. The recorder publishes
    /// a new immutable object only after the frame slot has completed; callers
    /// never wait for query availability and may keep the previous sample.
    struct RGGpuTimingSnapshot final
    {
        std::uint64_t frame_id{0};
        bool available{false};
        std::vector<RGGpuPassTiming> passes;
        double total_milliseconds{0.0};
    };

    /// Per-slot image + view injection for one frame-in-flight slot.
    struct ImportedSlotBinding
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    struct RGFrameContext
    {
        uint32_t frame_index = 0;
        uint32_t image_index = 0;
        uint64_t frame_id = 0;

        /// Scene (Set 0) descriptor set, injected by Renderer each frame.
        /// Applied to all passes that declared bindSceneDS(0).
        VkDescriptorSet scene_ds = VK_NULL_HANDLE;

        /// Index of the RenderScene's slot in the SceneGlobalGpuData[] SoA.
        /// Pushed as push_constant at offset 0 before each graphics pass.
        uint32_t scene_index = 0;

        /// Index of the View's slot in the ViewGpuData[] SoA.
        /// Pushed as push_constant at offset 4 before each graphics pass.
        uint32_t view_index = 0;

        /// Pointer to the active View for this recording invocation.
        /// Set by Renderer::renderView(); available in PassRecordContext::view.
        /// 这是一个**不透明透传**:图层只把它转交给上层(特性在 PassRecordContext
        /// 里用),自己从不解引用 —— 故本头只前置声明 View,不 include 它。
        const View* view = nullptr;

        /// 本次录制所属视图的视锥平面,已按 GPU 期望的布局打包成中性字节。
        ///
        /// 为什么给字节而不是让图去读 View:视锥是**上层的领域数据**,而渲染图
        /// 是通用机器。由组装方(Renderer)决定"搬哪些字节",图层只负责搬 ——
        /// 这样 L2 既不认识 View,尺寸也不必再硬编码(旧实现写死 6*16)。
        /// 空 span = 本次录制没有视锥可传,消费者应跳过上传。
        std::span<const std::byte> view_frustum{};

        /// 0 = first view for this scene in the current frame (barriers are correct
        ///     as compiled).  >0 = subsequent view; the recorder patches the first-
        ///     touch barrier of every imported resource so its src stage/access/layout
        ///     reflects the previous view's final state instead of the declared
        ///     initial state.
        uint32_t cross_view_index = 0;

        /// Per-slot image/view overrides.  When a slot entry has a non-null image,
        /// the recorder patches the corresponding physical resource before rendering.
        std::array<ImportedSlotBinding, kTargetSlotCount> imported_slots{};

        /// 当前录制所渲往 target 的 layout(binding.layout;format_key
        /// 拆分)。final_layout/usage 不再参与模板编译键,录制期按它参数化
        /// 图末 final barrier 的 newLayout 与后续帧首触屏障的 oldLayout——
        /// 同一模板可渲往 final_layout 各异的多个 target。null = 沿用编译
        /// 期烙定值(headless/无 target 路径)。
        const RenderTargetLayout* target_layout = nullptr;

        // ---- Generic extension data slots ----
        /// Per-slot data pointers set by features/kernels via FrameExtensionRegistry.
        /// Lifetime: feature-owned memory, valid for the current frame's recording.
        std::array<const void*, kMaxFrameExtensionSlots> ext_data{};

        // ---- External (e.g. CUDA-produced) timeline-semaphore sync ----
        /// Injected by a feature in populateFrameContext via
        /// addExternalGraphicsWait/Signal; the Renderer folds these into the
        /// frame's GRAPHICS submit (graphics waits before sampling an imported image;
        /// signals after, for ping-pong back to the producer). Empty for all normal
        /// rendering — the downstream merge is then a no-op (zero overhead/regression).
        std::vector<VkSemaphoreSubmitInfo> external_graphics_waits;
        std::vector<VkSemaphoreSubmitInfo> external_graphics_signals;
    };

    // ========== Multi-queue submission info (A-01) ==========

    /// Describes a single queue submission for multi-queue rendering.
    struct RGQueueSubmission
    {
        ERGQueueType queue_type = ERGQueueType::GRAPHICS;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        /// Timeline semaphore wait info (Vulkan sync2 style).
        std::vector<VkSemaphoreSubmitInfo> wait_semaphores;
        /// Timeline semaphore signal info (Vulkan sync2 style).
        std::vector<VkSemaphoreSubmitInfo> signal_semaphores;
    };

    /// Result of multi-queue recording.  Contains ordered submissions for all queues.
    /// The caller must submit these to the appropriate queues using vkQueueSubmit2
    /// in the order they appear in `submissions`.
    struct RGMultiQueueSubmitInfo
    {
        /// Ordered list of submissions.  Submit in order; timeline semaphores handle sync.
        std::vector<RGQueueSubmission> submissions;
        /// Whether multi-queue recording was actually used.
        bool is_multi_queue = false;
    };

    struct RGRecordContext
    {
        uint32_t frames_in_flight{0};
        bool use_dynamic_rendering{false};
        // layout group index -> VkRenderPass (empty when use_dynamic_rendering == true)
        std::vector<VkRenderPass> render_passes;

        // [frame_index][group_index] -> VkFramebuffer
        std::vector<std::vector<VkFramebuffer>> framebuffers;
        // group_index -> render area
        std::vector<VkExtent2D> group_extents;
        // group_index -> layerCount for VkRenderingInfo (>1 for TEX_2D_ARRAY attachments)
        std::vector<uint32_t> group_layer_counts;
        // resource_index -> whole-image VkImageView (Local RT)
        std::vector<VkImageView> attachment_views;
        // Extra view list, for automatic destruction of multi-frame resources or non-cached views
        std::vector<VkImageView> extra_views;
        // Pre-created per-frame attachment views [resource_index][frame_index]
        // Populated by allocateRecordContext so PassRecordContext::resolveTextureView
        // is a pure table lookup in the hot path.
        // For mip_levels>1 resources this is the FULL-mip view (sampling the whole chain).
        std::vector<std::vector<VkImageView>> per_frame_views;

        // Pre-created per-frame PER-MIP views [resource_index][frame_index][mip_level].
        // Only populated for resources with mip_levels > 1 (e.g. the HZB pyramid's
        // per-level storage/sample views for the downsample chain).
        std::vector<std::vector<std::vector<VkImageView>>> per_frame_views_by_mip;

        // Per-frame image overrides for imported slots [resource_index][frame_index].
        // Populated at the start of record() from RGFrameContext::imported_slots.
        // Barrier and view-resolution code checks this table before physical_resources.
        std::vector<std::vector<VkImage>> per_frame_images;

        // Per-frame buffer handles [resource_index][frame_index].
        // Populated by allocateRecordContext for buffer resources; used by
        // barrier patching at record time instead of physical_resources.
        std::vector<std::vector<VkBuffer>> per_frame_buffers;

        // ========== Binding state tracking (per frame, for optimization across record() calls) ==========
        // [frame_index] -> binding state (Note: only meaningful during incremental recording)
        std::vector<BindingState> binding_states;

        // ========== Per-frame scratch buffers (reused across record() calls) ==========
        // Avoids heap allocation inside the hot record() path.
        std::vector<uint8_t> processed_passes_scratch;
        /// 条件快照(record() 开头一帧一判):同帧对同一 pass 多次求值
        /// condition 可能因外部状态翻转而分裂("跳过判定"与"义务登记"
        /// 不一致)。所有分支只读快照,"一帧一个决定"是接口契约。
        std::vector<uint8_t> cond_skip_scratch;
        std::vector<VkImageMemoryBarrier2> image_barrier_scratch;
        std::vector<VkBufferMemoryBarrier2> buffer_barrier_scratch;

        // ========== Multi-queue support (A-01) ==========
        VkCommandPool compute_cmd_pool = VK_NULL_HANDLE;
        VkCommandPool transfer_cmd_pool = VK_NULL_HANDLE;
        VkSemaphore timeline_semaphore = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> compute_cmd_bufs;  ///< [frame_index]
        std::vector<VkCommandBuffer> transfer_cmd_bufs; ///< [frame_index]
        RGMultiQueueSubmitInfo multi_queue_submit;      ///< Populated by record()

        // ========== Transient descriptor sets (per-view per-frame) ==========
        VkDescriptorPool transient_ds_pool = VK_NULL_HANDLE;
        // [transient_ds_index][frame_index] -> VkDescriptorSet
        std::vector<std::vector<VkDescriptorSet>> transient_descriptor_sets;

        // ========== Physical resource back-pointer (set at record() start) ==========
        const RGPhysicalResourceTable* physical_resources_ptr = nullptr;

        // ========== 运行时布局参数化(set at record() start)==========
        /// 当前录制的 target layout(来自 RGFrameContext::target_layout)。
        const RenderTargetLayout* target_layout = nullptr;
        /// slot → imported 资源索引 LUT(指向 RGCompiledGraph::slot_resource_idx
        /// 存储,kTargetSlotCount 项;屏障补丁按它反查资源所属槽位)。
        const uint32_t* slot_resource_idx = nullptr;

        // ========== Non-blocking per-pass GPU timestamps ==========
        // One disjoint range per frame-in-flight slot. Results are adopted when
        // that slot is reused after its fence has retired (normally >=3 frames),
        // before vkCmdResetQueryPool records the next sample.
        VkQueryPool timestamp_pool = VK_NULL_HANDLE;
        uint32_t timestamp_pass_capacity{0};
        float timestamp_period_nanoseconds{0.0f};
        std::vector<uint64_t> timestamp_frame_ids;
        std::vector<uint64_t> timestamp_result_scratch;
        std::shared_ptr<const RGGpuTimingSnapshot> latest_gpu_timing;
        std::deque<std::shared_ptr<const RGGpuTimingSnapshot>> gpu_timing_history;
    };

    /// Per-view resource state: physical GPU resources + recording context.
    /// Each View owns one of these; allocated on first render, resized as needed.
    struct RGResourceState
    {
        VkExtent2D current_extent{0, 0};
        RGPhysicalResourceTable physical_resources;
        RGRecordContext record_ctx;
    };

    class ResourceRegistry;

    class LUX_FUNCTION_PUBLIC RGRecorder
    {
    public:
        RGRecorder() = default;
        virtual ~RGRecorder() = default;
        RGRecorder(RGRecorder&&) noexcept = default;
        RGRecorder& operator=(RGRecorder&&) noexcept = default;
        RGRecorder(const RGRecorder&) = delete;
        RGRecorder& operator=(const RGRecorder&) = delete;

        virtual Expected<RGRecordContext> allocateRecordContext(
            const RGCompiledGraph& compiled_graph,
            const RGPhysicalResourceTable& physical_resources,
            VkExtent2D extent,
            uint32_t frames_in_flight
        ) = 0;

        virtual bool deallocateRecordContext(RGRecordContext& record_context) = 0;

        /// Refresh dynamic imported handles before rendering this resource state.
        /// Must be called after upload phase and before record().
        virtual void
        refreshDynamicImportedResources(RGResourceState& resource_state, const RGCompiledGraph& compiled_graph) = 0;

        virtual void record(
            RGResourceState& resource_state,
            const RGCompiledGraph& compiled_graph,
            const RGFrameContext& context,
            VkCommandBuffer target_cmd,
            ResourceRegistry* gpu_mgr = nullptr
        ) = 0;
    };
}
