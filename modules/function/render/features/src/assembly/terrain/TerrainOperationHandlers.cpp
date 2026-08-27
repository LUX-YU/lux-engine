#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/TerrainOperation.ops.hpp>
#include <lux/engine/render/renderer/features/terrain/TerrainResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cstdint>

namespace lux::render
{
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    void handleTerrainPageUpload(GeneralRenderServer::Dispatcher::Ctx& context, const UploadTerrainPagePayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene ? scene->sceneRegistry().find<TerrainResources>() : nullptr;
        const auto bytes = resolveExternalData(context.program, payload.page_data);
        std::uint32_t status = 1u;
        std::uint32_t slot = 0xffffffffu;
        if (resources)
        {
            if (!resources->accepts(payload.id, payload.revision))
            {
                status = 0u;
                if (const auto* page = resources->find(payload.id))
                    slot = page->cache_slot;
            }
            else if (resources->upsert(payload, bytes))
            {
                status = 0u;
                slot = resources->find(payload.id)->cache_slot;
            }
        }
        replyToCurrent<UploadTerrainPagePayload>(
            context,
            TerrainPageUploadedReply{payload.id, payload.revision, slot, status}
        );
    }

    void handleTerrainPageRemove(GeneralRenderServer::Dispatcher::Ctx& context, const RemoveTerrainPagePayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene ? scene->sceneRegistry().find<TerrainResources>() : nullptr;
        const auto status = resources && resources->remove(payload.id, payload.revision) ? 0u : 1u;
        replyToCurrent<RemoveTerrainPagePayload>(
            context,
            TerrainPageRemovedReply{payload.id, payload.revision, status, 0u}
        );
    }

    void handleTerrainPageCacheStats(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const QueryTerrainPageCacheStatsPayload& payload
    )
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene ? scene->sceneRegistry().find<TerrainResources>() : nullptr;
        replyToCurrent<QueryTerrainPageCacheStatsPayload>(
            context,
            resources ? resources->stats() : TerrainPageCacheStatsReply{}
        );
    }
} // namespace lux::render
