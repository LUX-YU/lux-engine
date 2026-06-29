#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapOperation.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapFeature.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // FeatureOpRegistrar / ServerOp
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // ── Uniform factory interface ────────────────────────────────────────

    static FeatureHandle tonemapCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        TonemapCommConfig cc{};
        if (param && param_size >= sizeof(TonemapCommConfig))
            cc = *static_cast<const TonemapCommConfig*>(param);

        TonemapFeature::Config cfg{};
        cfg.vertex_shader   = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.tone_map_op     = static_cast<EToneMapOperator>(cc.tone_map_op);
        cfg.exposure        = cc.exposure;
        cfg.gamma           = cc.gamma;
        return sc->addFeature<TonemapFeature>(cfg);
    }

    // Typed-op: one Param op. The registrar registers it via the shared
    // registerFeatureParamsOp and derives param_set_op_index from its position.
    using TonemapOps = FeatureOpRegistrar<ServerOp<TonemapParamsOp>>;

    const FeatureFactory kTonemapFeatureFactory{
        &tonemapCreateFn,
        &TonemapOps::registerAll,
        &TonemapOps::unregisterAll,
        "Tonemap",
        TonemapOps::kParamSetOpIndex,   // = 0, auto-derived (no magic number)
    };

} // namespace lux::render
