// ============================================================================
//  Grid2DOperationHandlers.cpp — Grid2D 的手写残余:op 语义函数
//  (createFn/registrar/factory/Proxy 由 comm/genops/Grid2DOperation.ops.cpp
//   生成并 extern 引用本函数 —— 少定义即链接错误。)
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/features/grid/Grid2DOperation.hpp>
#include <lux/engine/render/renderer/features/grid/Grid2DPassFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    void handleGrid2DSetParams(GeneralRenderServer::Dispatcher::Ctx& ctx,
                               const Grid2DSetParamsPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        if (sc)
            if (auto* f = sc->getFeatureAs<Grid2DPassFeature>(p.feature))
                f->setGrid2DParams({p.cellSize, p.majorEvery, p.linePx, p.onTop});
    }

} // namespace lux::render
