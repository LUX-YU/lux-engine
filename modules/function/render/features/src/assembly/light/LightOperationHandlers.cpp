// ============================================================================
//  LightOperationHandlers.cpp — LightFeature factory + feature-scoped light
//  CRUD commands. The create/update/destroy/batch handlers live HERE (a feature),
//  not in the core RenderServer dispatcher, and are registered with DYNAMIC
//  TypeIds via register_ops_fn (the grid pattern). createLight REPLIES with the
//  new RLightHandle (replyToCurrent, request-id-correlated). The core protocol
//  no longer names light.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>       // Dispatcher, Ctx, replyToCurrent, FeatureFactory
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp> // typed-op register/unregister
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/render/renderer/features/light/LightFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <lux/engine/render/resources/lighting/LightResources.hpp> // submit/modify/remove, LightHandle

#include <cstdint>
#include <span>
#include <utility>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

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
            return sc ? sc->resources().find<LightResources>() : nullptr;
        }

        void eraseLightBinding(RenderScene& scene, RenderEntityId entity) noexcept
        {
            auto& entities = scene.entities();
            if (!entities.valid(entity))
            {
                return;
            }
            entities.remove<LightBinding>(entity);
            if (!entities.all_of<MeshBinding>(entity))
            {
                entities.destroy(entity);
            }
        }

        void applyLightUpsert(Ctx& ctx, const UpsertLightPayload& payload)
        {
            auto* scene = lookupScene(ctx.user_state, payload.scene_id);
            auto* lights = resolveLights(ctx, payload.scene_id);
            if (scene == nullptr || lights == nullptr)
            {
                ctx.markDispatchError(renderError<err::resource::NotFound>());
                return;
            }

            auto& entities = scene->entities();
            if (auto* binding = entities.valid(payload.entity) ? entities.try_get<LightBinding>(payload.entity)
                                                                : nullptr)
            {
                lights->modify(handle_cast<LightHandle>(binding->light), fromLightPayload(payload));
                return;
            }

            auto created = lights->submit(fromLightPayload(payload));
            if (!created)
            {
                ctx.markDispatchError(renderError<err::resource::ModifyFailed>());
                return;
            }
            if (payload.transition_milliseconds > 0U)
            {
                (void)lights->beginFadeIn(
                    *created,
                    scene->sceneTime(),
                    static_cast<float>(payload.transition_milliseconds) / 1000.0F
                );
            }
            if (!entities.valid(payload.entity))
            {
                (void)entities.create(payload.entity);
            }
            entities.emplace<LightBinding>(
                payload.entity,
                RLightHandle{created->index, created->gen}
            );
        }
    } // anonymous namespace (helpers)

    void handleUpsertLight(GeneralRenderServer::Dispatcher::Ctx& ctx, const UpsertLightPayload& payload)
    {
        applyLightUpsert(ctx, payload);
    }

    void handleRemoveLight(GeneralRenderServer::Dispatcher::Ctx& ctx, const RemoveLightPayload& payload)
    {
        auto* scene = lookupScene(ctx.user_state, payload.scene_id);
        auto* binding = scene && scene->entities().valid(payload.entity)
            ? scene->entities().try_get<LightBinding>(payload.entity)
            : nullptr;
        if (scene == nullptr || binding == nullptr)
        {
            return;
        }
        if (auto* light_res = resolveLights(ctx, payload.scene_id))
        {
            const auto handle = handle_cast<LightHandle>(binding->light);
            const float duration = static_cast<float>(payload.transition_milliseconds) / 1000.0F;
            if (!light_res->beginFadeOut(handle, scene->sceneTime(), duration))
                light_res->remove(handle);
        }
        eraseLightBinding(*scene, payload.entity);
    }

    void handleLightStats(GeneralRenderServer::Dispatcher::Ctx& ctx, const LightStatsPayload& payload)
    {
        const auto* lights = resolveLights(ctx, payload.scene_id);
        replyToCurrent<LightStatsPayload>(
            ctx,
            lights
                ? LightStatsReply{
                      lights->lightCount(
                          ELightSetBindings::LIGHT_DIRECTIONAL
                      ),
                      lights->lightCount(ELightSetBindings::LIGHT_POINT),
                      lights->lightCount(ELightSetBindings::LIGHT_SPOT),
                      lights->lightCount(ELightSetBindings::LIGHT_AREA),
                      lights->transitionCount()}
                : LightStatsReply{}
        );
    }

    void handleLightBatch(GeneralRenderServer::Dispatcher::Ctx& ctx, std::span<const UpsertLightPayload> entries)
    {
        for (const auto& entry : entries)
        {
            applyLightUpsert(ctx, entry);
        }
    }

} // namespace lux::render
