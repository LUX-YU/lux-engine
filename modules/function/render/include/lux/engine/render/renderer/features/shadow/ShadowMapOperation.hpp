#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/core/EShadowTechnique.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapTypes.hpp>
#include <lux/engine/render/renderer/features/FeatureParamsOperation.hpp>   // SetFeatureParamsPayload
#include <lux/engine/render/renderer/features/FeatureOps.hpp>               // EOpKind / FeatureOpDesc
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    // =========================================================================
    //  Default shader name constants for ShadowMapFeature
    // =========================================================================
    // Depth-only _vp vert (vUV only) matching shadow_depth.frag's interface
    // exactly — see comment on kMeshShadowVertShaderName for the rationale
    // (the previous "mesh_shadow.vert" defaulted to the fat EVSM-caster vert
    // and tripped the validation layer's OutputNotConsumed warning).
    inline constexpr std::string_view kShadowMapVertShaderName = "shadow_depth_vp.vert";
    inline constexpr std::string_view kShadowMapFragShaderName = "shadow_depth.frag";

    /// Comm-layer config for ShadowMapFeature.
    /// Client fills shader indices from CompileShader replies.
    struct ShadowMapCommConfig
    {
        ShaderHandle shadow_vertex_shader{};
        ShaderHandle shadow_fragment_shader{};
        uint32_t atlas_page_resolution{kDefaultShadowAtlasPageResolution};
        uint32_t atlas_page_count{kDefaultShadowAtlasPageCount};
        uint32_t max_shadow_slices{kDefaultMaxShadowSlices};
        uint32_t enable_directional_csm{0}; // 0: single-slice directional shadow, 1: CSM
        float non_directional_shadow_max_distance{60.0f}; // <=0: no distance limit
        /// Initial shadow technique selection. The active technique can be
        /// switched at runtime via `ShadowMapFeature::setActiveTechnique`.
        /// PCF (current default) is depth-compare based and needs per-light
        /// shadow_bias tuning; EVSM is pre-filtered and bias-free. See
        /// .internal/plan/evsm-shadow-implementation-guide.md.
        EShadowTechnique default_technique{EShadowTechnique::PCF};
        uint8_t _pad[3]{};
        /// EVSM-specific knobs. Ignored under PCF. Capped to RGBA16F's
        /// representable range (exp(c·1) ≤ sqrt(65504) ≈ 255 → c ≤ 5.54).
        /// RGBA32F atlas can push these higher (Frostbite uses 40/5).
        float evsm_pos_exponent{5.0f};
        float evsm_neg_exponent{5.0f};
        float evsm_bleed_reduction{0.2f};
        uint32_t evsm_atlas_page_count{4};   ///< RGBA16F × 3 atlases at this page count
    };
    static_assert(std::is_trivially_copyable_v<ShadowMapCommConfig>);

    /// ShadowMap's single op: the GENERIC reflected setParams (EOpKind::Param). The
    /// quality knobs (atlas resolution/count, slice budget, non-directional distance,
    /// directional-CSM toggle) live in the reflected ShadowQualityParams struct and are
    /// pushed as a snapshot blob through the shared FeatureParamsProxy → applyParams.
    /// The typed-op registrar registers it via registerFeatureParamsOp and auto-derives
    /// FeatureFactory.param_set_op_index — no per-feature register_ops_fn, no hand-counted
    /// index, no hardcoded op TypeIds. (Replaces the old UpdateShadowQuality /
    /// SetDirectionalCsm typed payloads.)
    struct ShadowMapParamsOp
    {
        using Payload = SetFeatureParamsPayload;
        static constexpr EOpKind kind = EOpKind::Param;
        static constexpr const char* name = "ShadowMapParams";
    };

    /// ShadowMap feature factory — registered at runtime via RegisterFeatureType.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kShadowMapFeatureFactory;

} // namespace lux::render
