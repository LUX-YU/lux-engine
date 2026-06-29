#pragma once
#include <cstdint>
#include <lux/engine/render/core/RenderObjectTypes.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>

namespace lux::render
{
    // =========================================================================
    //  GPU-Driven Mesh Rendering — Shared Constants
    // =========================================================================

    // NOTE: the old fixed `kMaxBuckets` cap was removed (2026-06). The GPU-driven
    // draw path is fully MDC-count-driven (dynamic); the variant-bucket count is a
    // CPU policy soft cap living in VariantBucketManager, not a GPU layout limit.

    /// Maximum number of visible entries per draw list (legacy finalize path).
    ///
    /// This limit is shared by cull/finalize shaders and C++ recorders.
    inline constexpr uint32_t kMaxDrawsPerBucket = 4096u;

    /// Number of geometry-kind lanes used by mesh indirect/count buffers.
    inline constexpr uint32_t kGeometryKindCount =
        static_cast<uint32_t>(EGeometryKind::Custom) + 1u;

    /// Sentinel used by cull shaders to mean "no geometry-kind filter".
    inline constexpr uint32_t kAnyGeometryKind = 0xFFFFFFFFu;

    // =========================================================================
    //  Shadow Bias-Group Constants
    // =========================================================================

    /// Maximum number of distinct bias groups for shadow draws.
    /// Slices sharing identical (bias, slope_bias) are merged into one group.
    inline constexpr uint32_t kMaxShadowBiasGroups = 8;

    /// Maximum draws per bias group (total capacity preserved: 8 × 16384 = 128 × 1024).
    inline constexpr uint32_t kMaxDrawsPerBiasGroup = 16384u;

    // =========================================================================
    //  Unified Mesh-Cull Push Constants (single source of truth)
    // =========================================================================
    //
    // C++ mirror of the `PC` block in `mesh_cull_unified.comp`. This 10-uint
    // layout used to be hand-duplicated in three places — MeshKernels.cpp (view
    // fill), ShadowKernels.cpp (shadow fill) and RGVulkanRecorder.cpp (replay) —
    // each filling fields positionally. That let the *view* fill silently put a
    // draw-LANE (MDC) count where a geometry-KIND count belongs: a lone skinned
    // instance (kind=1, mdc_count=1) was culled to nothing with no validation
    // error. Build this struct ONLY via the factories below so the per-mode
    // semantics of the overloaded fields can't be transposed.
    //
    // Fields 5/6/9 are overloaded by CULL_MODE (spec constant 0 in the shader):
    //   VIEW   → field5 = geometry-kind count (kGeometryKindCount)
    //   SHADOW → field5 = cascade/cube slice count,
    //            field6 = bias-group count,
    //            field9 = view MDC count
    struct MeshCullPushConstants
    {
        uint32_t slot_count;                          // patched per-frame (instance slot count)
        uint32_t max_draws_per_bucket;                // SHADOW: kMaxDrawsPerBiasGroup; VIEW: 0
        uint32_t required_pass_mask;
        uint32_t required_geometry_kind;              // kAnyGeometryKind => no kind filter
        uint32_t geometry_kind_count_or_slice_count;  // VIEW: kGeometryKindCount; SHADOW: slice count
        uint32_t buckets_per_geometry_or_bias_groups; // VIEW: 0; SHADOW: bias-group count
        uint32_t enabled_geometry_mask;
        uint32_t extension_flags;
        uint32_t reserved_input0_or_view_mdc_count;   // SHADOW: view MDC count; VIEW: unused
        uint32_t reserved_input1;
        // World-partition active-mask GPU address (buffer-device-address), 8-byte
        // aligned at offset 40. 0 = no mask bound (large-world disabled) → the shader
        // skips the dormant-cell early-out and treats every instance as active.
        // Replaces the old fixed descriptor binding 8. PATCHED PER-FRAME by the
        // recorder from MeshInstanceExtData (the ring address rotates each frame), so
        // the factories below seed it to 0.
        uint64_t active_mask_addr;
    };
    static_assert(sizeof(MeshCullPushConstants) == 10u * sizeof(uint32_t) + sizeof(uint64_t),
                  "MeshCullPushConstants must match the PC block in mesh_cull_unified.comp "
                  "(10 uints + a 64-bit active-mask buffer address)");

    /// VIEW-mode cull push constants.
    /// `geometry_kind_count_or_slice_count` is set to the number of EGeometryKind
    /// values (kGeometryKindCount), NOT a draw-lane / MDC count — passing the
    /// latter silently culls every instance whose kind >= that count.
    /// `slot_count` is patched per-frame by the recorder.
    [[nodiscard]] inline MeshCullPushConstants makeViewCullPushConstants(
        uint32_t required_pass_mask,
        uint32_t enabled_geometry_mask,
        uint32_t extension_flags) noexcept
    {
        return MeshCullPushConstants{
            /*slot_count*/                          0u,
            /*max_draws_per_bucket*/                0u,
            /*required_pass_mask*/                  required_pass_mask,
            /*required_geometry_kind*/              kAnyGeometryKind,
            /*geometry_kind_count_or_slice_count*/  kGeometryKindCount,
            /*buckets_per_geometry_or_bias_groups*/ 0u,
            /*enabled_geometry_mask*/               enabled_geometry_mask,
            /*extension_flags*/                     extension_flags,
            /*reserved_input0_or_view_mdc_count*/   0u,   // unused in view mode
            /*reserved_input1*/                     0u,
            /*active_mask_addr*/                    0ull, // patched per-frame by recorder
        };
    }

    /// SHADOW-mode cull push constants. `slot_count`, the slice count and the
    /// bias-group count default to 0 here and are populated by the shadow kernel's
    /// per-dispatch setup; `view_mdc_count` addresses the shadow MDC table.
    [[nodiscard]] inline MeshCullPushConstants makeShadowCullPushConstants(
        uint32_t required_pass_mask,
        uint32_t enabled_geometry_mask,
        uint32_t extension_flags,
        uint32_t view_mdc_count) noexcept
    {
        return MeshCullPushConstants{
            /*slot_count*/                          0u,
            /*max_draws_per_bucket*/                kMaxDrawsPerBiasGroup,
            /*required_pass_mask*/                  required_pass_mask,
            /*required_geometry_kind*/              kAnyGeometryKind,
            /*geometry_kind_count_or_slice_count*/  0u,   // slice count (filled per-dispatch)
            /*buckets_per_geometry_or_bias_groups*/ 0u,   // bias-group count (filled per-dispatch)
            /*enabled_geometry_mask*/               enabled_geometry_mask,
            /*extension_flags*/                     extension_flags,
            /*reserved_input0_or_view_mdc_count*/   view_mdc_count,
            /*reserved_input1*/                     0u,
            /*active_mask_addr*/                    0ull, // patched per-frame by recorder
        };
    }
} // namespace lux::render
