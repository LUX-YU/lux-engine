#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{

    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        using Dispatcher = GeneralRenderServer::Dispatcher;
        using Ctx        = Dispatcher::Ctx;

        void handleSkyboxSetEquirect(Ctx& ctx, const SkyboxSetEquirectPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            if (sc)
                if (auto* f = sc->getFeatureAs<SkyboxFeature>(p.feature))
                    f->applyEquirectangularHandle(p.texture);
        }

        void handleSkyboxSetCubemap(Ctx& ctx, const SkyboxSetCubemapPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            if (sc)
                if (auto* f = sc->getFeatureAs<SkyboxFeature>(p.feature))
                    f->applyCubemapHandles(p.cube);
        }

    } // anonymous namespace

    // ── Uniform factory interface ────────────────────────────────────────

    static FeatureHandle skyboxCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        SkyboxCommConfig cc{};
        if (param && param_size >= sizeof(SkyboxCommConfig))
            cc = *static_cast<const SkyboxCommConfig*>(param);

        SkyboxFeature::Config cfg{};
        cfg.vertex_shader     = cc.vertex_shader;
        cfg.cubemap_fragment  = cc.cubemap_fragment;
        cfg.equirect_fragment = cc.equirect_fragment;

        // LDR pipeline → skybox renders on SceneColor directly
        if (sc->pipelineConfig().isLdr())
            cfg.color_input = "SceneColor";

        return sc->addFeature<SkyboxFeature>(cfg);
    }

    // Typed-op: register/unregister generated from the op list. The 2 handlers above are
    // the only hand-written pieces.
    using SkyboxOps = FeatureOpRegistrar<
        ServerOp<SkyboxSetEquirectOp, &handleSkyboxSetEquirect>,
        ServerOp<SkyboxSetCubemapOp,  &handleSkyboxSetCubemap>>;

    const FeatureFactory kSkyboxFeatureFactory{
        &skyboxCreateFn,
        &SkyboxOps::registerAll,
        &SkyboxOps::unregisterAll,
        "Skybox",
    };

    // =====================================================================
    //  SkyboxProxy — client-side proxy
    // =====================================================================

    void SkyboxProxy::setEquirect(
        RenderSceneId scene_id, FeatureHandle feature,
        RTextureHandle texture)
    {
        SkyboxSetEquirectPayload payload{};
        payload.scene_id = scene_id;
        payload.feature  = feature;
        payload.texture  = texture;
        send<SkyboxSetEquirectOp>(*session_, ops_, payload);
    }

    void SkyboxProxy::setCubemap(
        RenderSceneId scene_id, FeatureHandle feature,
        RTextureHandle cube)
    {
        SkyboxSetCubemapPayload payload{};
        payload.scene_id = scene_id;
        payload.feature  = feature;
        payload.cube     = cube;
        send<SkyboxSetCubemapOp>(*session_, ops_, payload);
    }

} // namespace lux::render
