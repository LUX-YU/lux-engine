// ============================================================================
//  LightOperationHandlers.cpp — LightFeature factory + feature-scoped light
//  CRUD commands. The create/update/destroy/batch handlers live HERE (a feature),
//  not in the core RenderServer dispatcher, and are registered with DYNAMIC
//  TypeIds via register_ops_fn (the grid pattern). createLight REPLIES with the
//  new RLightHandle (replyToCurrent, request-id-correlated). The core protocol
//  no longer names light.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>   // Dispatcher, Ctx, replyToCurrent, FeatureFactory
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // typed-op register/unregister
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>   // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/render/renderer/features/light/LightFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <lux/engine/render/resources/lighting/LightResources.hpp>   // submit/modify/remove, LightHandle

#include <cstdint>
#include <span>
#include <utility>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (resolves a scene from
    // the dispatcher user_state). Forward-declared — the grid-handler convention —
    // to avoid pulling the heavy RenderServerImpl.hpp into a feature.
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        //(本地那份 handle_cast 副本已删 —— 正版已归位到 L0 的
        // core/RenderResourceHandle.hpp。这是同一症状的第三例:skinning、light、
        // 以及 meshstack 各自"为了躲开服务端 Impl 头"复制了一份三行模板。
        // **同一段代码被抄三遍,说明它放错了地方,不说明抄得对。**)

        // Resolve a scene's LightResources via PUBLIC API only (lookupScene → scene
        // registry). Null when the scene has no LightFeature → caller no-ops.
        LightResources* resolveLights(Ctx& ctx, RenderSceneId scene_id)
        {
            auto* sc = lookupScene(ctx.user_state, scene_id);
            return sc ? sc->sceneRegistry().find<LightResources>() : nullptr;
        }
    } // anonymous namespace (helpers)

        // Create one light + REPLY with its RLightHandle. Inlines the old
        // GeneralRenderServer::createLight (find<LightResources> + submit), so the
        // handler needs no server-impl access.
    void handleCreateLight(GeneralRenderServer::Dispatcher::Ctx& ctx, const CreateLightPayload& p)
        {
            RLightHandle handle{};
            uint32_t     status = 1;   // 1 = failed (no LightFeature / submit error)
            if (auto* light_res = resolveLights(ctx, p.scene_id))
            {
                auto result = light_res->submit(fromLightPayload(p));
                if (result)
                {
                    const auto h = result.value();
                    handle = RLightHandle{h.index, h.gen};
                    if (p.transition_milliseconds > 0u)
                    {
                        if (auto* scene = lookupScene(ctx.user_state, p.scene_id))
                        {
                            (void)light_res->beginFadeIn(
                                h,
                                scene->sceneTime(),
                                static_cast<float>(p.transition_milliseconds) /
                                    1000.0f);
                        }
                    }
                    status = 0;
                }
            }
            replyToCurrent<CreateLightPayload>(ctx, LightCreatedReply{handle, status});
        }

    void handleUpdateLight(GeneralRenderServer::Dispatcher::Ctx& ctx, const UpdateLightPayload& p)
        {
            if (auto* light_res = resolveLights(ctx, p.scene_id))
                light_res->modify(handle_cast<LightHandle>(p.handle), fromUpdateLightPayload(p));
        }

    void handleDestroyLight(GeneralRenderServer::Dispatcher::Ctx& ctx, const DestroyLightPayload& p)
        {
            if (auto* light_res = resolveLights(ctx, p.scene_id))
            {
                auto* scene = lookupScene(ctx.user_state, p.scene_id);
                const auto handle = handle_cast<LightHandle>(p.handle);
                const float duration =
                    static_cast<float>(p.transition_milliseconds) / 1000.0f;
                if (!scene || !light_res->beginFadeOut(
                        handle,
                        scene->sceneTime(),
                        duration))
                {
                    light_res->remove(handle);
                }
            }
        }

    void handleLightStats(
        GeneralRenderServer::Dispatcher::Ctx& ctx,
        const LightStatsPayload& payload)
    {
        const auto* lights = resolveLights(ctx, payload.scene_id);
        replyToCurrent<LightStatsPayload>(
            ctx,
            lights
                ? LightStatsReply{
                      lights->lightCount(
                          ELightSetBindings::LIGHT_DIRECTIONAL),
                      lights->lightCount(ELightSetBindings::LIGHT_POINT),
                      lights->lightCount(ELightSetBindings::LIGHT_SPOT),
                      lights->lightCount(ELightSetBindings::LIGHT_AREA),
                      lights->transitionCount()}
                : LightStatsReply{});
    }

        // Batched update — N per-instance UpdateLightPayloads in one command. Each
        // entry carries its own scene_id; cache the last resolution since batches
        // are typically single-scene.
    void handleLightBatch(GeneralRenderServer::Dispatcher::Ctx& ctx, std::span<const UpdateLightPayload> entries)
        {
            LightResources* light_res = nullptr;
            RenderSceneId   cur{};
            bool            resolved = false;
            for (const auto& p : entries)
            {
                if (!resolved || p.scene_id != cur)
                {
                    cur       = p.scene_id;
                    light_res = resolveLights(ctx, cur);
                    resolved  = true;
                }
                if (light_res)
                    light_res->modify(handle_cast<LightHandle>(p.handle), fromUpdateLightPayload(p));
            }
        }

} // namespace lux::render
