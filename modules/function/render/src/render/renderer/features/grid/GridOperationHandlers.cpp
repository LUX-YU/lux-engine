#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridPassFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{

    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        using Dispatcher = GeneralRenderServer::Dispatcher;
        using Ctx        = Dispatcher::Ctx;

        void handleGridSetParams(Ctx& ctx, const GridSetParamsPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            if (sc)
                if (auto* f = sc->getFeatureAs<GridPassFeature>(p.feature.id))
                    f->setGridParams({p.planeY, p.cellSize, p.linePx, p.fadeDist, p.holeRatio});
        }

    } // anonymous namespace

    // ── Uniform factory interface ────────────────────────────────────────

    static uint32_t gridCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        GridCommConfig cc{};
        if (param && param_size >= sizeof(GridCommConfig))
            cc = *static_cast<const GridCommConfig*>(param);

        GridPassFeature::Config cfg{};
        cfg.vertex_shader   = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        return sc->addFeature<GridPassFeature>(cfg);
    }

    // Typed-op: the factory's register/unregister fns are generated from the op
    // declaration list (FeatureOps). handleGridSetParams above is the only
    // hand-written piece — the feature's actual semantics.
    using GridOps = FeatureOpRegistrar<ServerOp<GridSetParamsOp, &handleGridSetParams>>;

    const FeatureFactory kGridFeatureFactory{
        &gridCreateFn,
        &GridOps::registerAll,
        &GridOps::unregisterAll,
        "GridPass",
    };

    // =====================================================================
    //  GridProxy — client-side proxy
    // =====================================================================

    void GridProxy::setParams(
        RenderSceneId scene_id, FeatureHandle feature,
        float planeY, float cellSize, float linePx,
        float fadeDist, float holeRatio)
    {
        GridSetParamsPayload payload{};
        payload.scene_id  = scene_id;
        payload.feature   = feature;
        payload.planeY    = planeY;
        payload.cellSize  = cellSize;
        payload.linePx    = linePx;
        payload.fadeDist  = fadeDist;
        payload.holeRatio = holeRatio;
        send<GridSetParamsOp>(*session_, ops_, payload);
    }

} // namespace lux::render
