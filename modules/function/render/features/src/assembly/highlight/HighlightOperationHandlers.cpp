#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/features/highlight/HighlightOperation.hpp>
#include <lux/engine/render/renderer/features/highlight/HighlightFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <cstring>
#include <lux/engine/render/gpu/RenderContext.hpp>

namespace lux::render
{
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    void handleHighlightReplaceTargets(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                       const HighlightReplaceTargetsPayload& payload)
    {
        auto* scene = lookupScene(ctx.user_state, payload.scene_id);
        if (!scene)
        {
            return;
        }
        auto* feature = dynamic_cast<HighlightFeature*>(scene->getFeature(payload.feature));
        const auto bytes = resolveBlob(ctx.program, payload.targets);
        if (!feature)
        {
            return;
        }
        if (bytes.size() != payload.targets.size || bytes.size() % sizeof(RenderEntityId) != 0)
        {
            scene->renderContext().reportError(renderError<err::comm::BulkPayloadNotMultiple>(
                sizeof(RenderEntityId), bytes.size()), payload.scene_id.index, scene->frameSerial());
            return;
        }
        // memcpy accepts the transport blob's alignment; no borrowed pointer survives.
        std::vector<RenderEntityId> targets(bytes.size() / sizeof(RenderEntityId));
        if (!bytes.empty())
        {
            std::memcpy(targets.data(), bytes.data(), bytes.size());
        }
        feature->replaceTargets(std::move(targets));
    }

    Expected<FeatureHandle> HighlightCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<HighlightCommConfig>(param, param_size);
        if (!decoded)
        {
            return lux::cxx::unexpected(decoded.error());
        }
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
