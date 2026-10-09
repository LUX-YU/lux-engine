#pragma once

#include <cstdint>
#include <lux/cxx/container/HeterogeneousLookup.hpp>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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
            const RGCompiledGraph& compiled
        );

        /// Contribute mesh draw lanes to the MeshBucketLayoutPlan.
        /// Called once per compiled pass instance when building the lane list.
        /// Only needed for kernels that produce indirect draw lanes (e.g. MeshDraw).
        using MeshFn = void (*)(
            uint32_t pass_index,
            const RGCompiledPass& cpass,
            MeshBucketLayoutPlan& plan,
            PipelineManager& pipeline_manager
        );

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
        /// Called during command replay for patches with ESource::KERNEL_PATCH.
        /// @param source_param  Kernel-defined sub-source ID (low 8 bits of DynamicPatch::source_param)
        /// @param frame_ctx     Current frame context (provides ext_data for feature access)
        using PatchFn = uint32_t (*)(uint16_t source_param, const RGFrameContext& frame_ctx);

        EmitFn emit{nullptr};              ///< Non-null => CompiledNative fast path
        MeshFn contribute_mesh{nullptr};   ///< Non-null => lane list contribution
        ArenaFn contribute_arena{nullptr}; ///< Non-null => arena size contribution
        ReplayFn replay{nullptr};          ///< Non-null => kernel sub-command replay handler
        PatchFn resolve_patch{nullptr};    ///< Non-null => kernel DynamicPatch resolution

        /// Declaration input only. Registration resolves it through FrameExtensionRegistry
        /// and retains an owned name plus the resolved numeric slot.
        const char* ext_slot_name{nullptr};
    };

    struct KernelDeclaration final
    {
        std::string_view canonical_name;
        KernelDescriptor descriptor;
    };

    struct KernelRegistration final
    {
        std::string_view canonical_name;
        KernelDescriptor descriptor;
        /// Empty only for code linked for the entire process lifetime.
        std::shared_ptr<const void> code_lifetime;
    };

    enum class EKernelRegistrationError
    {
        INVALID_NAME,
        CONFLICT,
        CAPACITY,
        INVALID_EXTENSION_NAME,
        EXTENSION_CAPACITY
    };
    using KernelRegistrationResult = lux::cxx::expected<KernelTypeId, EKernelRegistrationError>;

    struct RegisteredKernel final
    {
        // Declared first: descriptor and its owned name disappear before the final code pin.
        std::shared_ptr<const void> code_lifetime;
        std::string extension_name;
        KernelDescriptor descriptor;
        FrameExtensionSlotId extension_slot{};
    };

    /// Serialized composition only; no unregister or hot replacement. Consumers must finish
    /// before process registry teardown. Entries have stable addresses; accepted code stays pinned.
    class LUX_FUNCTION_PUBLIC KernelRegistry final
    {
    public:
        static KernelRegistry& instance() noexcept;
        KernelRegistry(const KernelRegistry&) = delete;
        KernelRegistry& operator=(const KernelRegistry&) = delete;

        [[nodiscard]] KernelRegistrationResult registerKernel(KernelRegistration registration) noexcept;
        /// Successful prefixes remain registered and pinned if a later declaration is rejected.
        [[nodiscard]] lux::cxx::expected<void, EKernelRegistrationError> registerKernels(
            std::span<const KernelDeclaration>,
            std::shared_ptr<const void> code_lifetime = {}
        ) noexcept;

        [[nodiscard]] KernelTypeId idOf(std::string_view name) const noexcept;
        [[nodiscard]] const RegisteredKernel* find(KernelTypeId id) const noexcept;

        template <typename Fn> void forEach(Fn&& fn) const
        {
            const auto& keys = kernels_.keys();
            const auto& values = kernels_.values();
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                fn(keys[i], *values[i]);
            }
        }

    private:
        KernelRegistry() = default;
        lux::cxx::OffsetSparseSet<KernelTypeId, std::unique_ptr<RegisteredKernel>> kernels_;
        lux::cxx::heterogeneous_map<KernelTypeId> name_to_id_;
    };

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
