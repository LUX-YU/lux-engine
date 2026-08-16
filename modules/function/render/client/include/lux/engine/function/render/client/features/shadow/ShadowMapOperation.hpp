#pragma once
// ============================================================================
//  ShadowMapOperation.hpp — ShadowMap 通信外观的【作者声明】(A+)
//  单 param_op 特性(质量旋钮走反射 ShadowQualityParams 快照,共享
//  FeatureParamsProxy → applyParams)。custom_create:CommConfig 映射到
//  嵌套的 Config::shadow_config,非同名抄写留手写。requires=light:
//  阴影切片写的是 Light 集的域绑定,缺装明确拒绝。
//  Operation 面由 engine_add_comm_ops 生成;手写残余 = ShadowMapCreateFn
//  (ShadowMapOperationHandlers.cpp)。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/lighting/EShadowTechnique.hpp>
#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::render
{
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
    struct LUX_COMM_CONFIG(prefix=ShadowMap, id=lux.render.shadow_map.v1, display=ShadowMap,
                           custom_create=true, requires=lux.render.light.v1,
                           param_op=ShadowMapParams, param_lane=frame)
    ShadowMapCommConfig
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


    /// 本特性产出的 render-graph pass 名(跨 feature 引用请用常量)。
    /// Forward / MeshShadow / DeferredLighting 三家都要排在它之后。
    inline constexpr std::string_view kShadowViewUploadPassName = "ShadowViewUpload";
    /// EVSM 模糊的第二趟(由 EVSMShadowTechnique 产出,与阴影图集同属阴影链)。
    /// DeferredLighting 采 EVSM 图集前要排在它之后。
    inline constexpr std::string_view kEvsmBlurVPassName = "EVSMBlurV";
} // namespace lux::render
