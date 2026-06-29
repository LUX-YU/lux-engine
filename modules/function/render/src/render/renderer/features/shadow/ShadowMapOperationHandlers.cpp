#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapFeature.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // FeatureOpRegistrar / ServerOp
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // ── Uniform factory interface ────────────────────────────────────────

    static FeatureHandle shadowMapCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        ShadowMapCommConfig cc{};
        if (param && param_size >= sizeof(ShadowMapCommConfig))
            cc = *static_cast<const ShadowMapCommConfig*>(param);

        ShadowMapFeature::Config cfg{};
        cfg.shadow_config.vertex_shader    = cc.shadow_vertex_shader;
        cfg.shadow_config.fragment_shader  = cc.shadow_fragment_shader;
        cfg.shadow_config.atlas_page_resolution = cc.atlas_page_resolution;
        cfg.shadow_config.atlas_page_count      = cc.atlas_page_count;
        cfg.shadow_config.max_shadow_slices     = cc.max_shadow_slices;
        cfg.shadow_config.enable_directional_csm = cc.enable_directional_csm;
        cfg.shadow_config.non_directional_shadow_max_distance =
            cc.non_directional_shadow_max_distance;
        cfg.shadow_config.default_technique     = cc.default_technique;
        cfg.shadow_config.evsm_pos_exponent     = cc.evsm_pos_exponent;
        cfg.shadow_config.evsm_neg_exponent     = cc.evsm_neg_exponent;
        cfg.shadow_config.evsm_bleed_reduction  = cc.evsm_bleed_reduction;
        cfg.shadow_config.evsm_atlas_page_count = cc.evsm_atlas_page_count;
        return sc->addFeature<ShadowMapFeature>(cfg);
    }

    // Typed-op: one Param op. The registrar registers it via the shared
    // registerFeatureParamsOp and derives param_set_op_index from its position; the
    // quality knobs are applied by ShadowMapFeature::applyParams (the shared
    // SetFeatureParams handler routes the reflected ShadowQualityParams blob there).
    using ShadowMapOps = FeatureOpRegistrar<ServerOp<ShadowMapParamsOp>>;

    // Stable type identity + descriptor (阶段 3). ShadowMap reads the scene's light
    // data, so it REQUIRES the Light feature; it contributes graph passes and owns
    // per-view state (per_view_shadow_ ViewStateTable). The dep array has static
    // storage (static constexpr) so the descriptor's span stays valid program-wide.
    static constexpr FeatureDependency kShadowMapDeps[] = {
        { featureId("lux.render.light.v1"), /*optional=*/false },
    };
    static constexpr FeatureDescriptor kShadowMapDescriptor{
        .type               = featureId("lux.render.shadow_map.v1"),
        .name               = "ShadowMap",
        .dependencies       = kShadowMapDeps,
        .contributes_graph  = true,
        .creates_view_state = true,
    };

    const FeatureFactory kShadowMapFeatureFactory{
        &shadowMapCreateFn,
        &ShadowMapOps::registerAll,
        &ShadowMapOps::unregisterAll,
        "ShadowMap",
        ShadowMapOps::kParamSetOpIndex,   // = 0, auto-derived (no magic number)
        kShadowMapDescriptor,
    };

} // namespace lux::render
