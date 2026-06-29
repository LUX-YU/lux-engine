#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/features/highlight/HighlightOperation.hpp>
#include <lux/engine/render/renderer/features/highlight/HighlightFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    static FeatureHandle highlightCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        HighlightCommConfig cc{};
        if (param && param_size >= sizeof(HighlightCommConfig))
            cc = *static_cast<const HighlightCommConfig*>(param);

        HighlightFeature::Config cfg{};
        cfg.cull_shader               = cc.cull_shader;
        cfg.compact_shader            = cc.compact_shader;
        cfg.mask_vert                 = cc.mask_vert;
        cfg.mask_frag                 = cc.mask_frag;
        cfg.blur_frag                 = cc.blur_frag;
        cfg.composite_frag            = cc.composite_frag;
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags           = cc.extension_flags;
        cfg.glow_color[0]             = cc.glow_color[0];
        cfg.glow_color[1]             = cc.glow_color[1];
        cfg.glow_color[2]             = cc.glow_color[2];
        cfg.glow_intensity            = cc.glow_intensity;
        cfg.glow_radius               = cc.glow_radius;
        return sc->addFeature<HighlightFeature>(cfg);
    }

    const FeatureFactory kHighlightFeatureFactory = makeSimpleFactory(&highlightCreateFn, "Highlight");

} // namespace lux::render
