#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/features/postprocess/FogOperation.hpp>
#include <lux/engine/render/renderer/features/postprocess/FogFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    void handleFogSetParams(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const FogSetParamsPayload& params)
    {
        if (auto* scene = lookupScene(
                context.user_state, params.scene_id))
        {
            if (auto* feature = scene->getFeatureAs<FogFeature>(
                    params.feature))
            {
                feature->setParams(params);
            }
        }
    }
} // namespace lux::render
