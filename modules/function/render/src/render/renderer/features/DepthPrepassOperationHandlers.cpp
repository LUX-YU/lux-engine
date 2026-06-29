#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/features/DepthPrepassOperation.hpp>
#include <lux/engine/render/renderer/features/DepthPrepassFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // ── Uniform factory interface ────────────────────────────────────────
    static uint32_t depthPrepassCreateFn(void* scene_ptr, const void* param, size_t param_size)
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

    const FeatureFactory kDepthPrepassFeatureFactory = makeSimpleFactory(&depthPrepassCreateFn);

} // namespace lux::render
