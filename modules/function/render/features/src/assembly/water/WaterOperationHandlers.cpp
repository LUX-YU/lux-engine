#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>
#include <lux/engine/render/renderer/features/water/WaterFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <span>

namespace lux::render
{
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        WaterFeature* resolveWater(
            GeneralRenderServer::Dispatcher::Ctx& context,
            RenderSceneId scene_id)
        {
            auto* scene = lookupScene(context.user_state, scene_id);
            if (!scene)
                return nullptr;
            for (auto* feature : scene->features())
            {
                if (feature && feature->name() == "Water")
                    return static_cast<WaterFeature*>(feature);
            }
            return nullptr;
        }
    } // namespace

    void handleWaterSurfaceCreate(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const WaterSurfaceCreatePayload& payload)
    {
        auto* water = resolveWater(context, payload.scene_id);
        const auto reply = water
            ? water->createSurface(
                  payload.surface)
            : WaterSurfaceCreatedReply{{}, 1u};
        replyToCurrent<WaterSurfaceCreatePayload>(context, reply);
    }

    void handleWaterSurfaceUpdate(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const WaterSurfaceUpdatePayload& payload)
    {
        if (auto* water = resolveWater(context, payload.scene_id))
            water->updateSurface(payload.handle, payload.surface);
    }

    void handleWaterSurfaceBatch(
        GeneralRenderServer::Dispatcher::Ctx& context,
        std::span<const WaterSurfaceUpdatePayload> payloads)
    {
        WaterFeature* water = nullptr;
        RenderSceneId scene{};
        bool resolved = false;
        for (const auto& payload : payloads)
        {
            if (!resolved || payload.scene_id != scene)
            {
                scene = payload.scene_id;
                water = resolveWater(context, scene);
                resolved = true;
            }
            if (water)
                water->updateSurface(payload.handle, payload.surface);
        }
    }

    void handleWaterSurfaceDestroy(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const WaterSurfaceDestroyPayload& payload)
    {
        if (auto* water = resolveWater(context, payload.scene_id))
            water->destroySurface(payload.handle);
    }

    void handleWaterStats(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const WaterStatsPayload& payload)
    {
        const auto* water = resolveWater(context, payload.scene_id);
        replyToCurrent<WaterStatsPayload>(
            context,
            water ? water->stats() : WaterStatsReply{});
    }
} // namespace lux::render
