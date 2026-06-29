#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <limits>
#include <array>
#include <string>

#include <lux/cxx/container/SmallVector.hpp>

#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGDepAnalyzer.hpp>
#include <lux/engine/render/graph/RenderPassPlanner.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/render/pipeline/RenderPassKey.hpp>
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>
#include <vulkan/vulkan.h>

namespace lux::render
{
    // =====================================================================
    //  Compilation Products (Phase 1+)
    //
    //  These structs are populated by new compiler stages added in Phase 1.
    //  During Phase 0 they are declared as empty placeholders so the
    //  infrastructure compiles and features can begin using setKernel().
    // =====================================================================

    /// An ordered lane within the mesh bucket layout.
    /// Each lane maps to one vkCmdDrawIndexedIndirectCount call.
    struct MeshLane
    {
        uint32_t    lane_id{0};
        uint32_t    pass_index{0};          ///< Index into compiled_passes
        uint8_t     geometry_kind{0};       ///< EGeometryKind value
        uint32_t    bucket_id{0};
        uint32_t    mdc_index{~0u};         ///< MDC entry index
        VkDeviceSize indirect_offset{0};    ///< Byte offset in per-view indirect buffer
        VkDeviceSize count_offset{0};       ///< Byte offset in per-view count buffer
        VkPipeline  pipeline{VK_NULL_HANDLE};
        bool        bind_pipeline{false};   ///< True when pipeline differs from previous lane
    };

    /// Compile-time mesh bucket layout: ordered lanes + buffer sizes.
    struct MeshBucketLayoutPlan
    {
        std::vector<MeshLane>   lanes;
        VkDeviceSize            total_indirect_size{0};
        VkDeviceSize            total_count_size{0};
    };

    /// A named region within the per-view arena buffer.
    struct ViewArenaRegion
    {
        uint32_t        region_id{0};
        VkDeviceSize    offset{0};
        VkDeviceSize    size{0};
        VkDeviceSize    alignment{0};
        VkBufferUsageFlags usage{0};
    };

    /// Per-view arena allocation blueprint.
    struct ViewAllocatorPlan
    {
        std::vector<ViewArenaRegion> regions;
        VkDeviceSize    total_arena_size{0};
        uint32_t        max_views{0};
    };

    /// Precompiled descriptor binding plan per pass.
    struct DescriptorBindingPlan
    {
        struct SlotBinding
        {
            uint32_t                slot{0};
            VkDescriptorSetLayout   layout{VK_NULL_HANDLE};
            enum class EUpdateFreq : uint8_t { Immutable, PerFIF, PerView };
            EUpdateFreq             frequency{EUpdateFreq::Immutable};
        };
        std::vector<SlotBinding>    bindings;
        uint32_t                    total_ds_count{0};  ///< FIF × MaxViews × bindings_per_view
    };

    /// Precompiled barrier sequence with handle-patch tables.
    struct BarrierProgram
    {
        struct BarrierGroup
        {
            uint32_t pass_index{0};
            std::vector<VkImageMemoryBarrier2>  image_barriers;
            std::vector<VkBufferMemoryBarrier2> buffer_barriers;
            std::vector<uint32_t>               image_patch_resource_idx;
            std::vector<uint32_t>               buffer_patch_resource_idx;
        };
        std::vector<BarrierGroup> first_view_barriers;
        std::vector<BarrierGroup> subsequent_view_barriers;
        std::vector<BarrierGroup> final_barriers;

        /// Precomputed pass→barrier-group index (sized to compiled_passes.size()).
        /// Value is the index into first_view_barriers / subsequent_view_barriers,
        /// or kInvalidSlotIdx if the pass has no pre-barrier group.
        std::vector<uint32_t> first_view_group_by_pass;
        std::vector<uint32_t> subsequent_view_group_by_pass;
    };

    /// Precompiled multi-queue submission template.
    struct QueueSubmitProgram
    {
        struct SubmissionTemplate
        {
            ERGQueueType                            queue{ERGQueueType::GRAPHICS};
            std::vector<VkSemaphoreSubmitInfo>      wait_templates;
            std::vector<VkSemaphoreSubmitInfo>      signal_templates;
        };
        std::vector<SubmissionTemplate> submissions;
    };

    /// Bytecode-style execution program — the fast-path replacement
    /// for recorder-lambda interpretation.
    struct ExecutionProgram
    {
        struct PassCommandSpan
        {
            uint32_t first_command{0};
            uint32_t one_past_last_command{0};
            bool     valid{false};
        };

        struct Command
        {
            enum class EType : uint8_t
            {
                SetPassContext,              ///< Stores current pass_index for subsequent commands
                BindPipeline,
                BindDescriptorSets,
                SetViewport,
                SetScissor,
                PushConstants,
                BeginRendering,
                EndRendering,
                Dispatch,
                DrawDirect,                 ///< vkCmdDraw (fullscreen quad, simple draws)
                DrawIndexedIndirectCount,
                BindMeshBuffers,            ///< Binds shared mesh VBO + IBO for indirect draws
                UploadViewFrustum,          ///< Uploads view frustum UBO from frame_ctx.view
                CullPushConstants,          ///< Push constants for GPU cull dispatch
                PipelineBarrier,
                FillBuffer,
                ClearCounters,
                InvokeKernelFn,            ///< Invoke pass kernel_fn with compiled bindings/context
                CopyBuffer,
                // ---- Generic kernel sub-command dispatch ----
                KernelCommand,              ///< Dispatched to KernelDescriptor::ReplayFn
            };
            EType       type{};
            uint32_t    data_offset{0};     ///< Offset into command_data (cumulative; grows with scene content -> must be 32-bit)
            uint16_t    data_size{0};       ///< Size of one command's arguments in command_data (per-command, bounded)
        };

        struct DynamicPatch
        {
            uint32_t    command_index{0};   ///< Index into commands[] (can exceed 65535 at MDC scale)
            uint16_t    data_field_offset{0};
            enum class ESource : uint8_t
            {
                FrameIndex,
                ViewIndex,
                SceneIndex,
                ImageHandle,
                BufferHandle,
                TimelineSemaphoreValue,
                KernelPatch,                ///< Resolved by KernelDescriptor::PatchFn (source_param encodes kernel ID + sub-source)
            };
            ESource     source{};
            uint16_t    source_param{0};    ///< e.g. resource_idx or slot index
        };

        std::vector<Command>        commands;
        std::vector<std::byte>      command_data;   ///< Flat command argument storage
        std::vector<DynamicPatch>   patches;        ///< Runtime handle/value patch table
        std::vector<PassCommandSpan> pass_spans;    ///< Indexed by compiled pass index
        bool                        full_fast_path{false};
        bool                        dynamic_rendering_compatible{false}; ///< true when no PipelineBarrier falls inside a rendering scope
    };
    // Native RenderPass / Framebuffer handle:
    // This is also just an index, real VkRenderPass / VkFramebuffer are managed by Vulkan layer using tables.
    struct RGNativeRenderPassHandle
    {
        uint32_t index{ std::numeric_limits<uint32_t>::max() };

        [[nodiscard]] bool is_valid() const noexcept
        {
            return index != std::numeric_limits<uint32_t>::max();
        }
    };

    struct RGNativeFramebufferHandle
    {
        uint32_t index{ std::numeric_limits<uint32_t>::max() };

        [[nodiscard]] bool is_valid() const noexcept
        {
            return index != std::numeric_limits<uint32_t>::max();
        }
    };

    /// @brief Barrier info (VK_KHR_synchronization2 style) with per-barrier stage masks.
    /// Used for both image and buffer barriers. Buffer barriers use offset/size;
    /// image barriers use oldLayout/newLayout/subresourceRange.
    struct RGBarrierInfo2 {
        uint32_t                resource_index; // Logical index
        VkPipelineStageFlags2   srcStageMask;
        VkAccessFlags2          srcAccessMask;
        VkPipelineStageFlags2   dstStageMask;
        VkAccessFlags2          dstAccessMask;
        VkImageLayout           oldLayout;
        VkImageLayout           newLayout;
        VkImageSubresourceRange subresourceRange;
        // Buffer-specific fields (unused for image barriers)
        VkDeviceSize            offset{0};
        VkDeviceSize            size{VK_WHOLE_SIZE};
    };

    /// Precompiled descriptor binding step for record hot-path.
    struct DSBindRecipe
    {
        using ResolveFn = VkDescriptorSet (*)(
            const DSBindRecipe& recipe,
            uint32_t frame_slot,
            VkDescriptorSet scene_ds,
            const std::vector<std::vector<VkDescriptorSet>>* transient_sets);

        uint32_t            slot{0};
        ResolveFn           resolve{nullptr};
        VkDescriptorSet     immutable_set{VK_NULL_HANDLE};
        DescriptorProvider  provider{};
        uint32_t            transient_ds_index{0};

        static VkDescriptorSet resolveImmutable(
            const DSBindRecipe& recipe,
            uint32_t /*frame_slot*/,
            VkDescriptorSet /*scene_ds*/,
            const std::vector<std::vector<VkDescriptorSet>>* /*transient_sets*/)
        {
            return recipe.immutable_set;
        }

        static VkDescriptorSet resolveScene(
            const DSBindRecipe& /*recipe*/,
            uint32_t /*frame_slot*/,
            VkDescriptorSet scene_ds,
            const std::vector<std::vector<VkDescriptorSet>>* /*transient_sets*/)
        {
            return scene_ds;
        }

        static VkDescriptorSet resolveTransient(
            const DSBindRecipe& recipe,
            uint32_t frame_slot,
            VkDescriptorSet /*scene_ds*/,
            const std::vector<std::vector<VkDescriptorSet>>* transient_sets)
        {
            if (transient_sets == nullptr)
                return VK_NULL_HANDLE;
            if (recipe.transient_ds_index >= transient_sets->size())
                return VK_NULL_HANDLE;
            const auto& per_frame = (*transient_sets)[recipe.transient_ds_index];
            if (frame_slot >= per_frame.size())
                return VK_NULL_HANDLE;
            return per_frame[frame_slot];
        }

        static VkDescriptorSet resolveResource(
            const DSBindRecipe& recipe,
            uint32_t frame_slot,
            VkDescriptorSet /*scene_ds*/,
            const std::vector<std::vector<VkDescriptorSet>>* /*transient_sets*/)
        {
            if (recipe.provider.resolver == nullptr)
                return VK_NULL_HANDLE;
            return recipe.provider.resolver(recipe.provider.resource, frame_slot);
        }
    };

    /// @brief Aggregates all synchronization / barrier information for a compiled pass.
    ///
    /// Extracted from RGCompiledPass to reduce cognitive load. Contains:
    ///   - Sync2 barriers (per-barrier stage masks)
    ///   - Split barriers (release/acquire halves, sync2 optimization)
    ///   - Cross-queue ownership transfer barriers
    ///   - Pre-built VkBarrier arrays: all static fields filled at compile time;
    ///     only the .image/.buffer handle is patched per frame at record time.
    struct RGPassSyncInfo
    {
        // --- Sync2 barriers (per-barrier stage masks) ---
        lux::cxx::SmallVector<RGBarrierInfo2, 4>         image_barriers_2;
        lux::cxx::SmallVector<RGBarrierInfo2, 4>         buffer_barriers_2;

        // (Split-barrier halves removed — see RenderGraphCompiler: two plain
        //  pipeline barriers with NONE/NONE intersecting scopes never chain;
        //  proper split barriers need VkEvent. Full barrier stays on consumer.)

        // --- Cross-queue ownership transfer barriers (QFOT) ---
        // RESERVED — not implemented. These are never populated: the RenderGraph
        // compiler's M4 fail-fast guard rejects any graph with a cross-queue resource
        // dependency before they would be filled, and the recorder emits every barrier
        // with VK_QUEUE_FAMILY_IGNORED. Kept as the wiring site for future multi-queue
        // (ASYNC_COMPUTE / ASYNC_TRANSFER) support. See RenderGraphCompiler item M4.
        lux::cxx::SmallVector<RGBarrierInfo2, 2>         release_ownership_barriers;
        lux::cxx::SmallVector<RGBarrierInfo2, 2>         acquire_ownership_barriers;
        uint32_t                                         src_queue_family = VK_QUEUE_FAMILY_IGNORED;
        uint32_t                                         dst_queue_family = VK_QUEUE_FAMILY_IGNORED;

        // --- Pre-built VkBarrier arrays (zero-copy hot path) ---
        // Filled by RenderGraphCompiler::buildPrebuiltBarriers().
        // At record time, only the .image / .buffer handle (= physical_resources[ri].getHandle(frame))
        // needs to be written; everything else is already correct.
        lux::cxx::SmallVector<VkImageMemoryBarrier2, 2>  prebuilt_image_barriers;  ///< mirrors image_barriers_2
        lux::cxx::SmallVector<VkBufferMemoryBarrier2, 2> prebuilt_buffer_barriers; ///< mirrors buffer_barriers_2
        lux::cxx::SmallVector<uint32_t, 4>               image_patch_resource_idx; ///< resource_index per image barrier
        lux::cxx::SmallVector<uint32_t, 4>               buffer_patch_resource_idx;///< resource_index per buffer barrier
    };

    // Compiled single Pass: All information needed for execution is aggregated here

    /// @brief Render pass, framebuffer, pipeline state and boundary flags.
    struct PassRenderState
    {
        RGNativeRenderPassHandle        render_pass;
        RGNativeFramebufferHandle       framebuffer;
        uint32_t                        subpass = 0;

        VkPipeline                      pipeline = VK_NULL_HANDLE;
        VkPipelineLayout                pipeline_layout = VK_NULL_HANDLE;
        uint32_t                        descriptor_set_count = 0;

        // Retained for per-item pipeline variant switching at draw time.
        GraphicsPipelineHandle          pipeline_template_handle;
        RenderPassKey                   render_pass_key{};
        // Indexed pipeline variants for this pass:
        //   0 = setPipeline(), 1..N = addPipeline() in declaration order.
        // Features should switch via PassRecordContext::bindPipelineVariant().
        lux::cxx::SmallVector<GraphicsPipelineHandle, 8> pipeline_variant_handles;
        lux::cxx::SmallVector<VkPipeline, 8>             pipeline_variants;
        lux::cxx::SmallVector<VkPipelineLayout, 8>       pipeline_variant_layouts;

        // Indexed geometry pipeline table — O(1) lookup by EGeometryType enum value
        static constexpr uint32_t kGeometryTypeCount = static_cast<uint32_t>(EGeometryType::COUNT);

        // Flat indexed table populated by RenderGraphCompiler — O(1) per lookup
        std::array<VkPipeline,       kGeometryTypeCount> geo_pipeline_table{};
        std::array<VkPipelineLayout, kGeometryTypeCount> geo_layout_table{};
        std::array<uint32_t,         kGeometryTypeCount> geo_set_count_table{};

        [[nodiscard]] VkPipeline pipelineForGeometry(EGeometryType type) const noexcept
        {
            return geo_pipeline_table[static_cast<uint32_t>(type)];
        }

        [[nodiscard]] VkPipelineLayout layoutForGeometry(EGeometryType type) const noexcept
        {
            return geo_layout_table[static_cast<uint32_t>(type)];
        }

        [[nodiscard]] uint32_t descriptorSetCountForGeometry(EGeometryType type) const noexcept
        {
            return geo_set_count_table[static_cast<uint32_t>(type)];
        }

        [[nodiscard]] VkPipeline pipelineVariant(uint32_t variant_index) const noexcept
        {
            return (variant_index < pipeline_variants.size())
                ? pipeline_variants[variant_index]
                : VK_NULL_HANDLE;
        }

        [[nodiscard]] VkPipelineLayout pipelineVariantLayout(uint32_t variant_index) const noexcept
        {
            return (variant_index < pipeline_variant_layouts.size())
                ? pipeline_variant_layouts[variant_index]
                : VK_NULL_HANDLE;
        }

        bool                            begin_render_pass = false;
        bool                            end_render_pass   = false;
        bool                            bind_pipeline     = false;

        /// Bitmask of Vulkan set indices that MUST be re-bound for this pass.
        /// Combines pipeline layout incompatibility AND DS handle identity changes.
        /// Computed by RenderGraphCompiler::computeDescriptorBindingPlan().
        /// Bit i set → framework must call vkCmdBindDescriptorSets for slot i.
        /// Default is all-ones (conservative: always bind).
        uint32_t                        bind_ds_mask = ~0u;

        /// Precompiled linear DS binding recipe, consumed directly by recorder.
        lux::cxx::SmallVector<DSBindRecipe, 8> ds_bind_recipe;
    };

    /// @brief Physical resource indices for read/write images/buffers + descriptor sets.
    struct PassResourceBindings
    {
        lux::cxx::SmallVector<uint32_t, 4>           read_images;
        lux::cxx::SmallVector<uint32_t, 4>           write_images;
        lux::cxx::SmallVector<uint32_t, 4>           read_buffers;
        lux::cxx::SmallVector<uint32_t, 4>           write_buffers;

        /// Authoritative per-pass resource binding map: (named slot, vk slot index).
        /// Derived from GraphicsPipelineTemplate::resource_slot_map at compile time.
        /// Empty for compute passes that manage their own descriptor binding.
        lux::cxx::SmallVector<std::pair<EDescriptorSetSlot, uint32_t>, kDescriptorSetCount>
                                                     pass_resource_bindings;
    };

    /// Classification for passes with a runtime condition lambda.
    /// Determined at compile time by classifyElectivePasses().
    enum class EElectiveKind : uint8_t
    {
        NONE,                ///< No condition lambda — always executed.
        INTRA_GROUP_ADDITIVE ///< Shares render-pass group with an unconditional pass;
                             ///< never the first-writer.  Safe to skip without
                             ///< breaking barriers or loadOps.
    };

    enum class EPassExecutionMode : uint8_t
    {
        RECORDER_FALLBACK = 0,
        COMPILED_NATIVE,
        COMPILED_CALLBACK,
    };

    struct RGCompiledPass
    {
        const RGPassDescription*        pass = nullptr;
        uint32_t                        pass_index = 0;

        PassRenderState                 render;
        PassResourceBindings            resources;
        RGPassSyncInfo                  sync;

        // Conditional execution support (pointer into RGPassDescription)
        PassConditionFn*                condition = nullptr;

        /// Elective pass classification (compile-time).
        /// NONE for unconditional passes; INTRA_GROUP_ADDITIVE for conditional
        /// passes whose group also contains at least one unconditional pass.
        EElectiveKind elective_kind = EElectiveKind::NONE;
        EPassExecutionMode execution_mode = EPassExecutionMode::RECORDER_FALLBACK;

        // ===== Cross-queue support =====
        ERGQueueType queue_type = ERGQueueType::GRAPHICS;

        struct QueueDependency
        {
            uint32_t              src_pass_index;
            VkSemaphore           semaphore;
            uint64_t              signal_value;
            VkPipelineStageFlags2 signal_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkPipelineStageFlags2 wait_stage   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        };
        lux::cxx::SmallVector<QueueDependency, 2> wait_dependencies;
        lux::cxx::SmallVector<QueueDependency, 2> signal_dependencies;

        // Forwarding helpers for convenience (used by pass processors)
        [[nodiscard]] VkPipeline pipelineForGeometry(EGeometryType type) const noexcept { return render.pipelineForGeometry(type); }
        [[nodiscard]] VkPipelineLayout layoutForGeometry(EGeometryType type) const noexcept { return render.layoutForGeometry(type); }
        [[nodiscard]] uint32_t descriptorSetCountForGeometry(EGeometryType type) const noexcept { return render.descriptorSetCountForGeometry(type); }
    };

    /**
     * @brief Group of passes that can be recorded in parallel using secondary
     *        command buffers on separate threads.
     *
     * Passes within the same RenderPass group that have no data dependency
     * (only ordering dependency via subpass index) can be recorded concurrently.
     * Independent RenderPass groups with no cross-resource dependency can also
     * be parallelized.
     */
    struct RGParallelGroup
    {
        lux::cxx::SmallVector<uint32_t, 8> pass_indices;      ///< Compiled pass indices that can be recorded in parallel
        uint32_t              render_pass_group;  ///< Owning VkRenderPass group index (UINT32_MAX for compute/independent)
        bool                  requires_secondary_cb{ false }; ///< True if the group needs secondary CBs
    };

    /**
     * @brief Multi-queue execution info computed by the compiler (A-01).
     *
     * When the render graph contains ASYNC_COMPUTE or ASYNC_TRANSFER passes,
     * the compiler partitions the execution order into per-queue sub-orders
     * and computes timeline-semaphore dependencies for cross-queue sync.
     */
    struct RGMultiQueueInfo
    {
        /// Per-queue execution orders (subsets of the full execution_order).
        std::vector<uint32_t> graphics_order;
        std::vector<uint32_t> compute_order;
        std::vector<uint32_t> transfer_order;

        /// Whether the graph contains any async work requiring multi-queue submission.
        bool has_async_work = false;
    };

    /// @brief Compile-time snapshot of an imported resource's state after a full
    ///        graph recording (including final barriers).
    ///
    /// In multi-view rendering the same compiled graph is recorded N times into one
    /// command buffer.  View 0's barriers are correct (they match the declared
    /// import_info initial state).  For view N>0 the resource is NOT in the initial
    /// state — it is in whatever state the previous recording left it.  This struct
    /// captures that deterministic post-recording state so the recorder can patch
    /// the first-touch barrier for each imported resource at record time.
    struct ImportedResourceFinalState
    {
        uint32_t                resource_index;
        VkPipelineStageFlags2   stage_mask;
        VkAccessFlags2          access_mask;
        VkImageLayout           layout;
    };

    // Result of the entire compiled RenderGraph
    struct RGCompiledGraph
    {
        RGCompiledGraph() = default;
        RGCompiledGraph(const RGCompiledGraph&) = delete;
        RGCompiledGraph& operator=(const RGCompiledGraph&) = delete;
        RGCompiledGraph(RGCompiledGraph&&) = default;
        RGCompiledGraph& operator=(RGCompiledGraph&&) = default;
        // Original RenderGraph description
        RGGraphDescription              original_graph;
        // Dependency analysis result (topological order + lifetime)
        RGDependencyInfo                dependency_info;
        // RenderPass layout (key for each graphics pass)
        RGRenderPassLayoutInfo          render_pass_layout;
        // Per-resource validity (true = format/usage legal, can be allocated).
        // Replaces physical_resources.contains() in all compiler phases.
        std::vector<bool>               valid_resources;
        // Compilation result for each pass
        std::vector<RGCompiledPass>     compiled_passes;
        // Execution order: index of compiled_passes, usually directly equal to dependency_info.pass_topological_order
        std::vector<uint32_t>           execution_order;

        // Parallel recording groups (computed by RenderGraphCompiler::computeParallelGroups)
        std::vector<RGParallelGroup>    parallel_groups;

        // Multi-queue execution info
        RGMultiQueueInfo                multi_queue_info;

		// Relative sized resources
        std::vector<uint32_t>           relative_size_resources;

		// Indices of resources with dynamic image_getter callbacks
		// Pre-computed during compilation for O(K) refresh instead of O(N)
		std::vector<uint32_t>           dynamic_external_resources;

        // Graph export barriers (e.g., Swapchain Present transition) — sync2 with per-barrier stages
        std::vector<RGBarrierInfo2>     final_barrier_infos_2;

        // Pre-built final barrier array — static fields filled at compile time;
        // only .image is patched per frame in record().
        std::vector<VkImageMemoryBarrier2>  prebuilt_final_barriers;
        std::vector<uint32_t>               final_patch_resource_idx;

        /// Sentinel value for slot_resource_idx entries that have no assignment.
        static constexpr uint32_t kInvalidSlotIdx = ~0u;

        /// Maps TargetSlot → logical resource index.
        /// Entries are kInvalidSlotIdx when the slot has no registered resource.
        /// Populated by RenderGraphCompiler when an imported resource carries a slot.
        std::array<uint32_t, kTargetSlotCount> slot_resource_idx{
            []() {
                std::array<uint32_t, kTargetSlotCount> a{};
                a.fill(~0u);
                return a;
            }()
        };

        /// Compile-time final state of each imported resource after one full
        /// graph recording + final barriers.  Used by multi-view recording to
        /// fix cross-view synchronisation on the first-touch barrier.
        std::vector<ImportedResourceFinalState> imported_final_states;

        /// resource_index → index into imported_final_states.
        /// Entries are kInvalidSlotIdx when the resource is not imported or untouched.
        std::vector<uint32_t>                  imported_final_state_lut;

        /// Human-readable compile diagnostic when valid == false.
        std::string                             compile_error;

        bool                            valid = false;

        // ===== Phase 1 compilation products (populated when all passes use kernels) =====
        std::optional<ExecutionProgram>         execution_program;
        std::optional<MeshBucketLayoutPlan>     mesh_bucket_layout;
        std::optional<ViewAllocatorPlan>        view_allocator_plan;
        std::optional<DescriptorBindingPlan>    descriptor_binding_plan;
        std::optional<BarrierProgram>           barrier_program;
        std::optional<QueueSubmitProgram>       queue_submit_program;

        /// Returns true when at least one pass has compiled execution commands.
        [[nodiscard]] bool hasFastPath() const noexcept { return execution_program.has_value(); }

        /// Returns true when every live pass can be replayed from the ExecutionProgram.
        [[nodiscard]] bool hasFullFastPath() const noexcept
        {
            return execution_program.has_value() && execution_program->full_fast_path;
        }
    };
} // namespace lux::render
