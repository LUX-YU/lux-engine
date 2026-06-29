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
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>  // send / sendWithReply / sendBulk
#include <lux/engine/render/comm/RenderProtocol.hpp>          // opcodes
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>
#include <lux/engine/render/renderer/features/light/LightFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>    // LightProxy: builder/RenderRequest

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
        // Trivial index/gen handle reinterpret (RLightHandle → LightHandle). Mirrors
        // RenderServerImpl.hpp's handle_cast; kept local so the feature stays
        // decoupled from the server impl header.
        template <typename To, typename From>
        To handle_cast(From h) noexcept { return To{h.index, h.gen}; }

        // Resolve a scene's LightResources via PUBLIC API only (lookupScene → scene
        // registry). Null when the scene has no LightFeature → caller no-ops.
        LightResources* resolveLights(Ctx& ctx, RenderSceneId scene_id)
        {
            auto* sc = lookupScene(ctx.user_state, scene_id);
            return sc ? sc->sceneRegistry().find<LightResources>() : nullptr;
        }

        // Create one light + REPLY with its RLightHandle. Inlines the old
        // GeneralRenderServer::createLight (find<LightResources> + submit), so the
        // handler needs no server-impl access.
        void handleCreateLight(Ctx& ctx, const CreateLightPayload& p)
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
                    status = 0;
                }
            }
            replyToCurrent<CreateLightPayload>(ctx, LightCreatedReply{handle, status});
        }

        void handleUpdateLight(Ctx& ctx, const UpdateLightPayload& p)
        {
            if (auto* light_res = resolveLights(ctx, p.scene_id))
                light_res->modify(handle_cast<LightHandle>(p.handle), fromUpdateLightPayload(p));
        }

        void handleDestroyLight(Ctx& ctx, const DestroyLightPayload& p)
        {
            if (auto* light_res = resolveLights(ctx, p.scene_id))
                light_res->remove(handle_cast<LightHandle>(p.handle));
        }

        // Batched update — N per-instance UpdateLightPayloads in one command. Each
        // entry carries its own scene_id; cache the last resolution since batches
        // are typically single-scene.
        void handleLightBatch(Ctx& ctx, std::span<const UpdateLightPayload> entries)
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
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static uint32_t lightCreateFn(void* scene_ptr, const void* /*param*/, size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        LightFeature::Config cfg{};   // LightFeature has no per-scene comm config
        return sc->addFeature<LightFeature>(cfg);
    }

    // Typed-op: register/unregister generated from the op list (kind picks
    // Unary/Bulk + opcode; the per-op opcode override carries update/destroy on
    // the ResourceOp ring). Handlers above are the only hand-written part. The
    // declared order == FeatureOpIds order on the client.
    using LightOps = FeatureOpRegistrar<
        ServerOp<CreateLightOp,  &handleCreateLight>,
        ServerOp<UpdateLightOp,  &handleUpdateLight>,
        ServerOp<DestroyLightOp, &handleDestroyLight>,
        ServerOp<LightBatchOp,   &handleLightBatch>>;

    const FeatureFactory kLightFeatureFactory{
        &lightCreateFn,
        &LightOps::registerAll,
        &LightOps::unregisterAll,
        "Light",
    };

    // ── Client-side proxy (typed-op send-routing: send/sendWithReply/sendBulk
    //    pick the push method + opcode + id from the op descriptor) ────────────
    RenderRequest<LightCreatedReply>
    LightProxy::createLight(RenderSceneId scene_id, const LightDescriptor& desc)
    {
        return sendWithReply<CreateLightOp>(*session_, ops_, toLightPayload(scene_id, desc));
    }

    void LightProxy::updateLight(RenderSceneId scene_id, RLightHandle handle, const LightDescriptor& desc)
    {
        send<UpdateLightOp>(*session_, ops_, toUpdateLightPayload(scene_id, handle, desc));
    }

    void LightProxy::updateLights(std::span<const UpdateLightPayload> entries)
    {
        sendBulk<LightBatchOp>(*session_, ops_, entries);
    }

    void LightProxy::destroyLight(RenderSceneId scene_id, RLightHandle handle)
    {
        DestroyLightPayload dlp{};
        dlp.scene_id = scene_id;
        dlp.handle   = handle;
        send<DestroyLightOp>(*session_, ops_, dlp);
    }

} // namespace lux::render
