// ============================================================================
//  ShadowMapOperationHandlers.cpp — ShadowMap 的手写残余:非同构 createFn
//  (registrar/descriptor/factory 由 comm/genops/ShadowMapOperation.ops.cpp
//   生成并 extern 引用本函数;Param op 走共享 registerFeatureParamsOp,
//   handler 语义为零。)
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // custom_create:CommConfig 逐字段映射到嵌套的 Config::shadow_config ——
    // 非同名抄写按 §7.5 留手写。
    Expected<FeatureHandle> ShadowMapCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<ShadowMapCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const ShadowMapCommConfig& cc = *decoded;

        ShadowMapFeature::Config cfg{};
        cfg.shadow_config.vertex_shader          = cc.shadow_vertex_shader;
        cfg.shadow_config.fragment_shader        = cc.shadow_fragment_shader;
        cfg.shadow_config.atlas_page_resolution  = cc.atlas_page_resolution;
        cfg.shadow_config.atlas_page_count       = cc.atlas_page_count;
        cfg.shadow_config.max_shadow_slices      = cc.max_shadow_slices;
        cfg.shadow_config.enable_directional_csm = cc.enable_directional_csm;
        cfg.shadow_config.non_directional_shadow_max_distance =
            cc.non_directional_shadow_max_distance;
        cfg.shadow_config.default_technique      = cc.default_technique;
        cfg.shadow_config.evsm_pos_exponent      = cc.evsm_pos_exponent;
        cfg.shadow_config.evsm_neg_exponent      = cc.evsm_neg_exponent;
        cfg.shadow_config.evsm_bleed_reduction   = cc.evsm_bleed_reduction;
        cfg.shadow_config.evsm_atlas_page_count  = cc.evsm_atlas_page_count;
        return sc->addFeature<ShadowMapFeature>(cfg);
    }

} // namespace lux::render
