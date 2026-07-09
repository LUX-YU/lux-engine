#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/features/DepthPrepassOperation.hpp>
#include <lux/engine/render/renderer/features/DepthPrepassFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // ── Uniform factory interface ────────────────────────────────────────
    static FeatureHandle depthPrepassCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        DepthPrepassCommConfig cc{};
        if (param && param_size >= sizeof(DepthPrepassCommConfig))
            cc = *static_cast<const DepthPrepassCommConfig*>(param);

        DepthPrepassFeature::Config cfg{};
        cfg.vertex_shader   = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        return sc->addFeature<DepthPrepassFeature>(cfg);
    }

    // Stable identity: re-registration (editor preview bring-up) dedups via
    // AlreadyRegistered instead of growing the registry per bring-up. (This factory
    // previously had no name at all — nothing looks a feature up by "", so giving it
    // its real name is purely additive.)
    static constexpr FeatureDescriptor kDepthPrepassDescriptor{
        .type              = featureId("lux.render.depth_prepass.v1"),
        .name              = "DepthPrepass",
        .contributes_graph = true,
    };
    const FeatureFactory kDepthPrepassFeatureFactory =
        makeSimpleFactory(&depthPrepassCreateFn, "DepthPrepass", kDepthPrepassDescriptor);

} // namespace lux::render
