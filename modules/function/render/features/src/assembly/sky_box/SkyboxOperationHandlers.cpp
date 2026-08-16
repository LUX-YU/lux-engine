// ============================================================================
//  SkyboxOperationHandlers.cpp — Skybox 的手写残余:op 语义 + 非同构 createFn
//  (registrar/factory/Proxy 由 comm/genops/SkyboxOperation.ops.cpp 生成并
//   extern 引用本文件的 handleSkyboxSet* 与 SkyboxCreateFn。)
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    void handleSkyboxSetEquirect(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                 const SkyboxSetEquirectPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        if (sc)
            if (auto* f = sc->getFeatureAs<SkyboxFeature>(p.feature))
                f->applyEquirectangularHandle(
                    p.texture,
                    p.rotation_radians,
                    p.intensity);
    }

    void handleSkyboxSetCubemap(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                const SkyboxSetCubemapPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        if (sc)
            if (auto* f = sc->getFeatureAs<SkyboxFeature>(p.feature))
                f->applyCubemapHandles(
                    p.cube,
                    p.rotation_radians,
                    p.intensity);
    }

    void handleSkyboxStats(
        GeneralRenderServer::Dispatcher::Ctx& ctx,
        const SkyboxStatsPayload& payload)
    {
        const auto* scene = lookupScene(ctx.user_state, payload.scene_id);
        const SkyboxFeature* skybox = nullptr;
        if (scene)
        {
            for (const auto* feature : scene->features())
            {
                if (feature && feature->name() == "Skybox")
                {
                    skybox = static_cast<const SkyboxFeature*>(feature);
                    break;
                }
            }
        }
        replyToCurrent<SkyboxStatsPayload>(
            ctx,
            skybox ? skybox->stats() : SkyboxStatsReply{});
    }

    // custom_create:同名字段抄写之外还有装配决策(LDR 管线时 skybox 直画
    // SceneColor)—— 非同构逻辑按 §7.5 留手写。
    Expected<FeatureHandle> SkyboxCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<SkyboxCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const SkyboxCommConfig& cc = *decoded;

        SkyboxFeature::Config cfg{};
        cfg.vertex_shader     = cc.vertex_shader;
        cfg.cubemap_fragment  = cc.cubemap_fragment;
        cfg.equirect_fragment = cc.equirect_fragment;

        // LDR pipeline → skybox renders on SceneColor directly
        if (sc->pipelineConfig().isLdr())
            cfg.color_input = "SceneColor";

        return sc->addFeature<SkyboxFeature>(cfg);
    }

} // namespace lux::render
