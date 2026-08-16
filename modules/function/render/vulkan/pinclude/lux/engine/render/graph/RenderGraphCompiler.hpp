#pragma once

#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/render/graph/DependencyAnalyzer.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/render/graph/RenderPassPlanner.hpp>

namespace lux::render
{
    class PipelineManager; // Forward declaration

    // Compile options
    class SceneDomainDescriptorSets;

    struct RGCompileOptions
    {
        bool allow_aliasing = true;          // Whether to allow physical resource aliasing
        bool merge_compatible_passes = true; // For future merge strategies
        bool dump_debug_info = false;        // For printing dot graph / debug

        /// The scene's domain-set instance (domain merge). For pipelines that
        /// have switched to the merged layout, the FEATURE domain slot is bound
        /// at record time to this instance (resolved per frames-in-flight
        /// slice) rather than to any single owner's per-set instance — the
        /// binding plan uses this to collapse several logical bindings into
        /// one domain bind. Null means this graph has no domain instance (a
        /// scene-less test graph); if a merged pipeline shows up in that case,
        /// the binding plan logs a plan warning and skips the slot, so a
        /// validation gate catches the problem instead of binding it wrong.
        const SceneDomainDescriptorSets* domain_sets = nullptr;

        /// Ceiling for the local-read merge budget: how many color attachments
        /// a merged render-pass group may union up to. 0 = "no device known",
        /// which falls back to RenderPassKey::kMaxColorAttachments.
        ///
        /// This must come from the device (DeviceCaps::max_color_attachments):
        /// Vulkan only guarantees 4, and a group wider than the device's limit
        /// fails at vkCmdBeginRendering. The old code used the ARRAY CAPACITY
        /// (8) as the policy ceiling — conflating "how much room the key struct
        /// has" with "how much the hardware allows", which is why nothing ever
        /// queried maxColorAttachments.
        uint32_t max_color_attachments = 0;
    };

    class LUX_FUNCTION_PUBLIC RenderGraphCompiler
    {
    public:
        static RGCompiledGraph compile(
            RGGraphDescription graph,
            PipelineManager &pipeline_manager,
            const RGCompileOptions &options = {});

    private:
        // 0) Resolve forward resource references — rewrite placeholder handles
        //    to actual resource indices before any other analysis.
        static void resolveForwardReferences(RGCompiledGraph &compiled);

        // 0.5) Inject bindResourceDS-declared external consumption (declared_consume)
        //      as READ texture/buffer refs so dependency analysis, ordering and
        //      dead-pass elimination see it — without rewriting the DS bind itself,
        //      which is still resolved by the feature at record time.
        static void injectResourceDSDependencies(RGCompiledGraph &compiled);

        // 1.5) Auto-fill transient-DS image layouts from each binding pass's
        //      resource role (via determineTextureState) so the descriptor layout
        //      always matches what computeBarriers transitions the image to —
        //      eliminating hand-written, drift-prone layouts.
        static void autoFillTransientDSLayouts(RGCompiledGraph &compiled);

        // 1.6) Warn (phase A) about bindResourceDS bindings that don't declare the
        //      RG resource they consume — invisible to ordering/dead-pass.
        static void validateResourceDSBindings(RGCompiledGraph &compiled);

        // 1) Global analysis: dependencies, lifetimes, resource validity, render pass layout
        static bool buildGlobalInfo(RGCompiledGraph &compiled, const RGCompileOptions &options);

        // 1.7) Descriptor-layout allocation at graph-compile time.
        //      Walks the whole graph — pass -> pipeline template -> per-binding
        //      reflection — aggregating the union across the whole graph to
        //      produce the full shape of each logical resource, emitting a
        //      LayoutPlan and writing it back to the template via
        //      PipelineManager::finalizeTemplateLayout.
        //      Must run before buildCompiledPasses (2): that step builds the
        //      PSO and snapshots tmpl.pipeline_layout into the compiled pass.
        //      On failure, writes compiled.compile_error and returns false —
        //      RenderScene's build-then-commit keeps the last good graph
        //      (a zero-side-effect rollback).
        static bool computeGraphDescriptorLayouts(RGCompiledGraph &compiled,
                                                  PipelineManager &pipeline_manager);

        // 2) Build compiled info for each pass
        static bool buildCompiledPasses(RGCompiledGraph &compiled, PipelineManager &pipeline_manager);
        static bool buildSinglePass(RGCompiledGraph &compiled, uint32_t pass_index, PipelineManager &pipeline_manager);

        // 2.1 Graphics pass: render pass / framebuffer / pipeline
        static void setupGraphicsPass(RGCompiledGraph &compiled, uint32_t pass_index,
                                      RGCompiledPass &cpass, RGPassDescription &pass_desc,
                                      PipelineManager &pipeline_manager);

        // 2.1b Compute pass: pipeline layout / descriptor info (DESIGN-01)
        static bool setupComputePass(RGCompiledGraph &compiled, RGCompiledPass &cpass, RGPassDescription &pass_desc,
                                     PipelineManager &pipeline_manager);

        // 2.2 Resource mapping: logical -> physical
        static void mapPassTextures(const RGCompiledGraph &compiled, RGCompiledPass &cpass);
        static void mapPassBuffers(const RGCompiledGraph &compiled, RGCompiledPass &cpass);

        // 3) Execution order
        static void computeExecutionOrder(RGCompiledGraph &compiled);

        // 3.1) Dead pass elimination — remove passes whose outputs contribute to no exported resource (A-02)
        /// Prune by INPUT: disable passes whose sources were never produced,
        /// transitively. Dual of eliminateDeadPasses (which prunes by output);
        /// run BEFORE it, since a starved pass must not keep its producers alive.
        static void eliminateStarvedPasses(RGCompiledGraph &compiled);
        static void eliminateDeadPasses(RGCompiledGraph &compiled);

        // 3.5) Queue assignment based on pass type
        static void computeQueueAssignment(RGCompiledGraph &compiled);

        // 3.6) Cross-queue dependencies: timeline semaphore values + ownership transfers (A-01)
        static void computeCrossQueueDependencies(RGCompiledGraph &compiled);

        // 4) RenderPass Begin/End
        static void computeRenderPassBoundaries(RGCompiledGraph &compiled);

        // ── 活 pass 资源访问反查索引 ─────────────────────────────────────
        // dead-pass 消除后按 execution_order 构建一次,op 推导(9)与条件链
        // 验证(4.5)共同消费。各 phase 私建索引曾造成语义漂移(含不含死
        // pass、声明序还是执行序)——F6/F12,统一收口在此。
        struct LiveAccessIndex
        {
            /// pass_idx → 执行序位置(UINT32_MAX = 不在执行序,即死 pass)。
            std::vector<uint32_t> order_of;
            /// res → 最后访问(读或写,image+buffer)的执行序位置。
            std::vector<uint32_t> last_access_order;
            /// res → 活读者 pass 列表(image+buffer)。
            std::vector<std::vector<uint32_t>> readers;
            /// res → 活 image 写者 pass 列表。
            std::vector<std::vector<uint32_t>> image_writers;
        };
        static LiveAccessIndex buildLiveAccessIndex(const RGCompiledGraph &compiled);

        // 4.5) Classify conditional passes as elective (intra-group additive)
        //      Must run before binding plan steps so they can conservatively
        //      reset state after elective passes.
        static bool classifyElectivePasses(RGCompiledGraph &compiled,
                                           const LiveAccessIndex &access);

        // 5) Pipeline binding strategy
        static void computePipelineBindingPlan(RGCompiledGraph &compiled);

        // 5.1) Descriptor set binding plan — per-slot layout-break mask.
        //      Must be called after computePipelineBindingPlan so that
        //      begin_render_pass flags are already set.
        /// Needs the PipelineManager: bindings marked `logical` must resolve
        /// their logical identity to an actual slot number using the slot
        /// table of whichever pipeline this pass uses (the same engine set
        /// can sit at a different slot in different pipelines).
        static void computeDescriptorBindingPlan(RGCompiledGraph &compiled,
                                                 PipelineManager &pipeline_manager,
                                                 const SceneDomainDescriptorSets *domain_sets);

        // 6) Compute Barriers
        static void computeBarriers(RGCompiledGraph &compiled);

        // (computeSplitBarriers removed: its release/acquire halves were plain
        //  pipeline barriers with empty intersecting scopes — they never chain
        //  per Vulkan's execution-dependency rule, dropping the dependency.
        //  Real split barriers require VkEvent; full barriers stay on the consumer.)

        // 6.6) Build pre-built VkBarrier arrays for the zero-copy hot path (P1)
        //      Must be called AFTER computeBarriers so all RGBarrierInfo2
        //      entries are finalised.
        static void buildPrebuiltBarriers(RGCompiledGraph &compiled);

        // 7) Compute Descriptor Sets
        static void computeDescriptorSets(RGCompiledGraph &compiled, PipelineManager &pipeline_manager);

        // 9) Pre-compute per-group / per-pass attachment load+store ops
        static void computeAttachmentOps(RGCompiledGraph &compiled,
                                        const LiveAccessIndex &access);

        // ===== Kernel-driven fast-path compiler stages =====

        // 11) Determine whether all graph passes use compile-time kernels,
        //     enabling the fast execution path.  Must be called after all
        //     existing stages so the compiled pass data is fully populated.
        static bool canBuildFastPath(const RGCompiledGraph &compiled);

        // 11.1) Build mesh bucket layout from MeshCull/MeshDraw kernel configs.
        //       Generates ordered lane list with compile-time buffer offsets.
        static void computeMeshBucketLayout(RGCompiledGraph &compiled, PipelineManager &pipeline_manager);

        // 11.2) Compute per-view arena allocation blueprint from bucket layout
        //       and frustum UBO/SSBO requirements.
        static void computeViewAllocatorPlan(RGCompiledGraph &compiled);

        // 11.3) Build precompiled barrier program: first-view, subsequent-view,
        //       and final barrier sequences with handle-patch tables.
        static void buildBarrierProgram(RGCompiledGraph &compiled);

        // 11.4) Build multi-queue submission template from cross-queue deps.
        static void buildQueueSubmitProgram(RGCompiledGraph &compiled);

        // 11.5) Build the bytecode-style execution program from all preceding
        //       compilation products.
        static void buildExecutionProgram(RGCompiledGraph &compiled);
    };

} // namespace lux::render
