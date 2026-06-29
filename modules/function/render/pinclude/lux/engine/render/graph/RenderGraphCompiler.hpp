#pragma once

#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGDepAnalyzer.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/render/graph/RenderPassPlanner.hpp>

namespace lux::render
{
    class PipelineManager; // Forward declaration

    // Compile options
    struct RGCompileOptions
    {
        bool allow_aliasing = true;          // Whether to allow physical resource aliasing
        bool merge_compatible_passes = true; // For future merge strategies
        bool dump_debug_info = false;        // For printing dot graph / debug
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
        //      which is still resolved by the feature at record time. (M3 layer 2)
        static void injectResourceDSDependencies(RGCompiledGraph &compiled);

        // 1.5) Auto-fill transient-DS image layouts from each binding pass's
        //      resource role (via determineTextureState) so the descriptor layout
        //      always matches what computeBarriers transitions the image to —
        //      eliminating hand-written, drift-prone layouts. (M3 layer 3)
        static void autoFillTransientDSLayouts(RGCompiledGraph &compiled);

        // 1.6) Warn (phase A) about bindResourceDS bindings that don't declare the
        //      RG resource they consume — invisible to ordering/dead-pass. (M3 layer 3)
        static void validateResourceDSBindings(const RGCompiledGraph &compiled);

        // 1) Global analysis: dependencies, lifetimes, resource validity, render pass layout
        static bool buildGlobalInfo(RGCompiledGraph &compiled, const RGCompileOptions &options);

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
        static void eliminateDeadPasses(RGCompiledGraph &compiled);

        // 3.5) Queue assignment based on pass type
        static void computeQueueAssignment(RGCompiledGraph &compiled);

        // 3.6) Cross-queue dependencies: timeline semaphore values + ownership transfers (A-01)
        static void computeCrossQueueDependencies(RGCompiledGraph &compiled);

        // 4) RenderPass Begin/End
        static void computeRenderPassBoundaries(RGCompiledGraph &compiled);

        // 4.5) Classify conditional passes as elective (intra-group additive)
        //      Must run before binding plan steps so they can conservatively
        //      reset state after elective passes.
        static bool classifyElectivePasses(RGCompiledGraph &compiled);

        // 5) Pipeline binding strategy
        static void computePipelineBindingPlan(RGCompiledGraph &compiled);

        // 5.1) Descriptor set binding plan — per-slot layout-break mask.
        //      Must be called after computePipelineBindingPlan so that
        //      begin_render_pass flags are already set.
        static void computeDescriptorBindingPlan(RGCompiledGraph &compiled);

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

        // 8) Compute parallel recording groups for multi-threaded CB recording
        static void computeParallelGroups(RGCompiledGraph &compiled);

        // 8.1) Force full bind for passes recorded in secondary CBs
        static void forceFullBindingForParallelPasses(RGCompiledGraph &compiled);

        // 9) Pre-compute per-group attachment resource indices and static loadOps
        static void computeGroupLoadOps(RGCompiledGraph &compiled);

        // ===== Phase 1 compiler stages (kernel-driven fast path) =====

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
