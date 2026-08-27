// ============================================================================
//  Grid3DOperationHandlers.cpp — Grid3D 的手写残余:op 语义函数
//
//  A+ 之后,createFn/registrar/factory/Proxy 全部由生成物
//  (comm/genops/Grid3DOperation.ops.cpp)提供,并 extern 引用本文件的
//  handleGrid3DSetParams —— 少定义即链接错误。本文件只回答一个问题:
//  「这个 op 对特性做什么」。
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/features/grid/Grid3DOperation.hpp>
#include <lux/engine/render/renderer/features/grid/Grid3DPassFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    void handleGrid3DSetParams(GeneralRenderServer::Dispatcher::Ctx& ctx, const Grid3DSetParamsPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        if (sc)
            if (auto* f = sc->getFeatureAs<Grid3DPassFeature>(p.feature))
                f->setGrid3DParams({p.planeY, p.cellSize, p.linePx, p.fadeDist, p.holeRatio, p.onTop});
    }

} // namespace lux::render
