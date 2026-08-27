#pragma once
/**
 * @file KernelDescriptor.hpp
 * @brief Open-closed kernel plugin system for the render graph compiler.
 *
 * A KernelDescriptor bundles compile-phase function pointers for a single
 * kernel.  Registering a descriptor with KernelRegistry tells the compiler
 * how to:
 *   - emit ExecutionProgram bytecode for the kernel (emit)
 *   - contribute lanes to the MeshBucketLayoutPlan (contribute_mesh, optional)
 *   - contribute arena regions to the ViewAllocatorPlan (contribute_arena, optional)
 *
 * To add a brand-new kernel (fully open-closed, zero enum modification needed):
 *   1. Create a *Kernels.cpp that defines the codegen functions and calls
 *      LUX_REGISTER_KERNEL("KernelName", desc).
 *   2. Add the new .cpp to the render CMakeLists.txt source list.
 *   Zero other files need to be modified. Works across DLL boundaries.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <lux/cxx/container/HeterogeneousLookup.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/SparseSet.hpp>

namespace lux::render
{
    // Forward declarations — keep this header light.
    struct ProgramEmitter;
    struct RGCompiledPass;
    struct RGCompiledGraph;
    struct MeshBucketLayoutPlan;
    struct RGPassDescription;
    struct KernelReplayContext;
    struct RGFrameContext;
    class PipelineManager;

    // =========================================================================
    // ViewArenaContribution
    // =========================================================================

    /// Accumulator passed to KernelDescriptor::ArenaFn during
    /// computeViewAllocatorPlan.  Each kernel that needs per-view GPU arena
    /// space increments the relevant fields.
    struct ViewArenaContribution
    {
        uint32_t frustum_ubo_count{0};  ///< One per MeshCull pass
        uint32_t shadow_slice_count{0}; ///< Max across all ShadowCull passes
    };

    // =========================================================================
    // KernelDescriptor
    // =========================================================================

    /**
     * @brief Compile-phase descriptor for a single kernel type.
     *
     * All function pointers are optional (nullptr = no-op for that phase).
     * A kernel with a non-null emit pointer will receive EPassExecutionMode::COMPILED_NATIVE
     * classification; otherwise it falls back to RecorderFallback.
     */
    struct KernelDescriptor
    {
        /// Emit the body commands for one pass into the ExecutionProgram.
        /// Called once per compiled pass instance of this kernel type.
        using EmitFn = void (*)(
            ProgramEmitter& emitter,
            uint32_t pass_index,
            const RGCompiledPass& cpass,
            const RGCompiledGraph& compiled);

        /// Contribute mesh draw lanes to the MeshBucketLayoutPlan.
        /// Called once per compiled pass instance when building the lane list.
        /// Only needed for kernels that produce indirect draw lanes (e.g. MeshDraw).
        using MeshFn = void (*)(
            uint32_t pass_index,
            const RGCompiledPass& cpass,
            MeshBucketLayoutPlan& plan,
            PipelineManager& pipeline_manager);

        /// Contribute to the per-view arena size estimates.
        /// Called once per *graph description* pass (not compiled pass) to
        /// accumulate region requirements into the contribution struct.
        using ArenaFn = void (*)(const RGPassDescription& pass_desc, ViewArenaContribution& accum);

        /// Replay a kernel-specific sub-command at command-buffer recording time.
        /// @param sub_cmd   Kernel-local sub-command index (0-based)
        /// @param data      Pointer to the sub-command payload in command_data
        /// @param data_size Payload byte count
        /// @param ctx       Replay context providing cmd buffer, frame data, physical resources
        using ReplayFn = void (*)(uint32_t sub_cmd, const void* data, uint16_t data_size, KernelReplayContext& ctx);

        /// Resolve a kernel-specific DynamicPatch source to a uint32_t value.
        /// Called during command replay for patches with ESource::KernelPatch.
        /// @param source_param  Kernel-defined sub-source ID (low 8 bits of DynamicPatch::source_param)
        /// @param frame_ctx     Current frame context (provides ext_data for feature access)
        using PatchFn = uint32_t (*)(uint16_t source_param, const RGFrameContext& frame_ctx);

        EmitFn emit{nullptr};              ///< Non-null => CompiledNative fast path
        MeshFn contribute_mesh{nullptr};   ///< Non-null => lane list contribution
        ArenaFn contribute_arena{nullptr}; ///< Non-null => arena size contribution
        ReplayFn replay{nullptr};          ///< Non-null => kernel sub-command replay handler
        PatchFn resolve_patch{nullptr};    ///< Non-null => kernel DynamicPatch resolution

        /// §3: Optional extension slot name. When non-null, registerKernel()
        /// auto-allocates a FrameExtensionSlot (deduped by name).
        const char* ext_slot_name{nullptr};
    };

    // =========================================================================
    // KernelRegistry
    // =========================================================================

    /**
     * @brief Global registry mapping kernel names to IDs and descriptors.
     *
     * Dual-indexed for optimal performance at each call site:
     *   - name_to_id_   : std::unordered_map — O(1) average for name→ID
     *                     (used at graph-build time via setKernel("name"))
     *   - descriptors_   : OffsetSparseSet   — O(1) for ID→Descriptor
     *                     (used on the hot compile/execute path via find(id))
     *
     * IDs are allocated sequentially starting from 1; 0 is kInvalidKernelId.
     * This keeps the OffsetSparseSet dense and cache-friendly regardless of
     * plugin registration order.
     *
     * Thread safety: all registration must complete before the first call to
     * RenderGraphCompiler::compile().  LUX_REGISTER_KERNEL() runs at static
     * initialisation (DLL/program load), which satisfies this requirement.
     *
     * Cross-DLL: KernelRegistry is exported (LUX_FUNCTION_PUBLIC).  Plugin
     * DLLs that call LUX_REGISTER_KERNEL share the single instance in the
     * render DLL rather than creating a local copy.
     */
    class LUX_FUNCTION_PUBLIC KernelRegistry
    {
    public:
        static KernelRegistry& instance() noexcept;

        /// Register a named kernel and return its assigned numeric ID.
        /// If the name was already registered the descriptor is replaced and
        /// the original ID is returned (idempotent for reload scenarios).
        KernelTypeId registerKernel(std::string_view name, KernelDescriptor desc);

        /// Name→ID lookup.  O(1) average (unordered_map).
        /// Returns kInvalidKernelId when the name is not registered.
        [[nodiscard]] KernelTypeId idOf(std::string_view name) const noexcept;

        /// ID→Descriptor lookup.  O(1) (OffsetSparseSet).
        /// Returns nullptr when id == kInvalidKernelId or not registered.
        [[nodiscard]] const KernelDescriptor* find(KernelTypeId id) const noexcept;

        /// §3: Extension slot name→ID lookup.  Returns kInvalidExtSlot when not registered.
        [[nodiscard]] FrameExtensionSlotId extSlotOf(std::string_view name) const noexcept;

        /// Dense iteration over all registered descriptors.
        /// Fn signature: void(KernelTypeId, const KernelDescriptor&)
        template <typename Fn> void forEach(Fn&& fn) const
        {
            const auto& ks = descriptors_.keys();
            const auto& vs = descriptors_.values();
            for (std::size_t i = 0; i < ks.size(); ++i)
                fn(ks[i], vs[i]);
        }

    private:
        lux::cxx::OffsetSparseSet<KernelTypeId, KernelDescriptor> descriptors_;
        /// 异构查找:键存 std::string,查表直接吃 string_view,不构造临时 string。
        /// `idOf` / `extSlotOf` 标着 noexcept,而临时 string 是可能抛的堆分配 ——
        /// 普通 unordered_map 的写法在 OOM 时会直接 std::terminate 而非传播错误。
        lux::cxx::heterogeneous_map<KernelTypeId> name_to_id_;
        lux::cxx::heterogeneous_map<FrameExtensionSlotId> ext_name_to_slot_;
        KernelTypeId next_id_{1};
        FrameExtensionSlotId next_ext_slot_{1};
    };

    // =========================================================================
    // LUX_REGISTER_KERNEL macro
    // =========================================================================

    /**
     * @brief Self-registration helper.
     *
     * Place in a .cpp file at namespace scope:
     *   LUX_REGISTER_KERNEL("MeshDraw", KernelDescriptor{
     *       .emit            = &emitMeshDrawKernel,
     *       .contribute_mesh = &meshDrawContributeMesh,
     *   });
     *
     * The macro runs at DLL/program load time (static initialisation).
     * No central enum file needs modification — fully open-closed.
     * Works across DLL boundaries because KernelRegistry is DLL-exported.
     *
     * Three-level macro indirection forces pre-expansion of __COUNTER__
     * before the token-paste operator ## sees it (required for MSVC).
     */
#define LUX_REGISTER_KERNEL_PASTE(kernel_name, descriptor, uid)                                                        \
    namespace                                                                                                          \
    {                                                                                                                  \
        struct _KernelRegistrar_##uid                                                                                  \
        {                                                                                                              \
            _KernelRegistrar_##uid()                                                                                   \
            {                                                                                                          \
                ::lux::render::KernelRegistry::instance().registerKernel((kernel_name), (descriptor));                 \
            }                                                                                                          \
        } _kernel_registrar_instance_##uid;                                                                            \
    } // namespace

// Middle level: uid is NOT used with ##, so __COUNTER__ is pre-expanded here
#define LUX_REGISTER_KERNEL_IMPL(kernel_name, descriptor, uid) LUX_REGISTER_KERNEL_PASTE(kernel_name, descriptor, uid)

#define LUX_REGISTER_KERNEL(kernel_name, descriptor) LUX_REGISTER_KERNEL_IMPL(kernel_name, descriptor, __COUNTER__)

    /// Byte size of one VkDrawIndexedIndirectCommand — the stride the GPU-driven
    /// path uses to index an indirect buffer (`indirect_offset = mdc_index * this`).
    ///
    /// Lives here because the three users span two layers: the graph compiler (L2)
    /// and the mesh/shadow kernels (L3). All three already include this header, and
    /// it is the lowest one they share — it used to be spelled `= 20` separately in
    /// each of them, with nothing tying the three to each other or to Vulkan.
    ///
    /// Not written as `sizeof(VkDrawIndexedIndirectCommand)` because this header
    /// deliberately does not pull <vulkan/vulkan.h>. The equality IS asserted, in
    /// the one TU that has the complete type — see MeshKernels.cpp.
    inline constexpr std::uint32_t kIndirectCommandSize = 20;

    /// Threads per workgroup for the one-thread-per-element cull and compaction
    /// dispatches — the divisor in every `(count + N - 1) / N` group calculation
    /// the mesh, shadow and utility kernels perform.
    ///
    /// It mirrors `layout(local_size_x = 64)` in exactly three shaders:
    ///   assets/shaders/forward/mesh_cull_unified.comp      (mesh AND shadow cull —
    ///       ShadowKernels dispatches the same module, via MESH_CULL_UNIFIED_COMP)
    ///   assets/shaders/forward/mdc_compact.comp
    ///   assets/shaders/forward/clear_count_buffers.comp
    /// Change one side and the other must follow; too small a divisor leaves the
    /// tail of the array unprocessed, too large dispatches groups that do nothing.
    ///
    /// ⚠️ Other compute shaders also declare 64 — cluster_build/count/fill,
    /// pointcloud_culling, skin_compute. Those are independent choices that happen
    /// to coincide, NOT users of this constant. Do not fold them in: changing this
    /// value must not silently retune dispatches nobody looked at.
    inline constexpr std::uint32_t kCullDispatchWorkgroupSize = 64;

} // namespace lux::render
