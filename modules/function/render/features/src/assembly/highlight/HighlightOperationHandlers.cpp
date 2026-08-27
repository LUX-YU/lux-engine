#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/features/highlight/HighlightOperation.hpp>
#include <lux/engine/render/renderer/features/highlight/HighlightFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    Expected<FeatureHandle> HighlightCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<HighlightCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const HighlightCommConfig& cc = *decoded;

        HighlightFeature::Config cfg{};
        cfg.cull_shader = cc.cull_shader;
        cfg.compact_shader = cc.compact_shader;
        cfg.mask_vert = cc.mask_vert;
        cfg.mask_frag = cc.mask_frag;
        cfg.blur_frag = cc.blur_frag;
        cfg.composite_frag = cc.composite_frag;
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags = cc.extension_flags;
        cfg.glow_color[0] = cc.glow_color[0];
        cfg.glow_color[1] = cc.glow_color[1];
        cfg.glow_color[2] = cc.glow_color[2];
        cfg.glow_intensity = cc.glow_intensity;
        cfg.glow_radius = cc.glow_radius;
        return sc->addFeature<HighlightFeature>(cfg);
    }

} // namespace lux::render
