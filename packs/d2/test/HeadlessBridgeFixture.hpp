#pragma once
// ============================================================================
//  HeadlessBridgeFixture.hpp — GPU-free in-process render harness for the
//  ECS→render bridge regression tests (Gate -1 / G-09).
//
//  The bridges (PoolBridge / InstanceBridge / ParamBridge) talk to the renderer
//  ONLY through a RenderSession command channel + a FeatureRegistry op-id table.
//  Neither needs a GPU:
//    * RenderSession is a pure command builder + SPSC ring I/O (no Vulkan).
//    * The generic RenderServer<> drains that ring and dispatches through a
//      FrameDispatcher — the dispatcher is a plain vtable, GPU only lives inside
//      the real feature handlers.
//  So we stand up a real client RenderSession against a real (generic)
//  RenderServer whose dispatcher carries RECORDING handlers instead of the GPU
//  ones. The handlers capture every command and hand back a controlled reply.
//  The whole round-trip runs on ONE thread — submitFrame publishes, the fake
//  server drains+replies synchronously, pumpReplies fires the continuations.
//
//  This lets a test drive a real bridge and assert exactly which commands it
//  emitted (create/update/destroy, per-scene routing, failure handling) and how
//  it reacts to injected replies (go-live, drain, orphan cleanup) — the runtime
//  half of the G-01..G-05 + P0 teardown fixes that were otherwise compile-only.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>      // FrameDispatcher, RenderServer<>, ExecuteContext, replyToCurrent
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>
#include <lux/engine/render/comm/FrameProgram.hpp>             // RenderProgramChannel
#include <lux/engine/render/comm/RenderCommTypes.hpp>          // RenderChannelSync, FrameMemoryHints, opcodes
#include <lux/engine/render/renderer/features/FeatureOps.hpp>  // opcode_of_v
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>  // MeshStack ops/payloads/proxy ids
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>       // Skybox ops/payloads (PARAM)
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>  // Canvas2D v2 instance ops (2D bridges)
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>   // Sprite2DInstanceData / Sprite2DHandle
#include <lux/engine/render/comm/RenderProtocol.hpp>          // MeshUploadedReply
#include <lux/engine/render/core/RenderObjectTypes.hpp>       // RenderObjectHandle

#include <lux/engine/asset/AssetManager.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::bridgetest
{
    // ── Recorder: what the fake server saw + what replies it should hand back ──
    //
    // `by_op` keeps the raw payload bytes of every dispatched command keyed by the
    // op's debug name, so a test asserts on counts + decoded payloads. The reply
    // knobs let a test steer the server's answer (success vs failure, distinct
    // handles) without a bespoke handler per case.
    struct Recorder
    {
        std::unordered_map<std::string, std::vector<std::vector<std::byte>>> by_op;

        // CreateLight reply controls.
        std::uint32_t light_create_status{0};   ///< 0 = Ok; non-zero = creation failed
        std::uint32_t next_light_index{1};      ///< hand out distinct RLightHandle indices
        /// Simulate a generic dispatch failure: reply {status 0, NULL handle} (the default
        /// reply RenderRequest delivers on CommandFailedReply). Tests P1-3 — status-only
        /// validation would emplace a null-handle zombie.
        bool          light_create_null_handle{false};
        std::vector<lux::render::RLightHandle> created_lights;
        std::vector<lux::render::RLightHandle> destroyed_lights;

        // Mesh-stack reply controls (InstanceBridge).
        std::uint32_t mesh_upload_status{0};    ///< 0 = Ok; non-zero = upload failed
        std::uint32_t next_mesh_index{1};       ///< hand out distinct RMeshHandle indices
        /// Simulate a generic dispatch failure for uploadMesh: reply {status 0, NULL
        /// handle}. Tests P1-4 — status-only validation settles a ready-but-null entry
        /// that re-uploads every frame.
        bool          mesh_upload_null_handle{false};
        std::uint32_t next_object_index{1};     ///< hand out distinct RenderObjectHandle indices
        lux::render::MeshInstanceCreateStatus add_instance_status{
            lux::render::MeshInstanceCreateStatus::Ok};   ///< addMeshInstance outcome the server reports
        /// When true, hand back a NON-null object even on a non-Ok status. This is a
        /// SYNTHETIC shape (the real dispatch-failure wire shape is {Unknown, NULL object}
        /// — RenderRequest delivers a default-constructed reply on CommandFailedReply). It
        /// is forced only to DECOUPLE the InstanceBridge's `status != Ok || !object` guard
        /// halves, so a P1-2 test can prove the STATUS check ALONE rejects a would-be-live
        /// reply (otherwise the `!object` half would mask the status check).
        bool add_instance_object_on_failure{false};
        std::vector<lux::render::RenderObjectHandle> created_objects;

        // Canvas2D v2 reply controls (Sprite2DBridge).
        lux::render::ECanvas2DCreateStatus add_sprite_status{
            lux::render::ECanvas2DCreateStatus::Ok};   ///< AddSprite2D outcome the server reports
        std::uint32_t next_sprite_index{1};            ///< hand out distinct Sprite2DHandle indices
        std::vector<lux::render::Sprite2DHandle> created_sprites;
        std::vector<lux::render::Sprite2DHandle> removed_sprites;

        void record(const char* op, const void* data, std::size_t n)
        {
            const auto* p = static_cast<const std::byte*>(data);
            by_op[op].emplace_back(p, p + n);
        }

        [[nodiscard]] std::size_t count(const char* op) const
        {
            const auto it = by_op.find(op);
            return it == by_op.end() ? 0u : it->second.size();
        }

        template <class T>
        [[nodiscard]] T payload(const char* op, std::size_t i) const
        {
            T out{};
            const auto& bytes = by_op.at(op).at(i);
            std::memcpy(&out, bytes.data(), sizeof(T));
            return out;
        }
    };

    namespace detail
    {
        using Ctx = lux::render::ExecuteContext<>;

        // Recording handlers — the fake server's stand-ins for the GPU light
        // handlers. Signature matches FrameDispatcher::registerUnary's Fn param.
        inline void recCreateLight(Ctx& ctx, const lux::render::CreateLightPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("CreateLight", &p, sizeof(p));
            lux::render::LightCreatedReply reply{};
            if (rec->light_create_null_handle)
            {
                reply.status = 0;   // dispatch-failure default reply: status 0 + null handle
            }
            else
            {
                reply.status = rec->light_create_status;
                if (reply.status == 0)
                {
                    reply.handle = lux::render::RLightHandle{ rec->next_light_index++, 1u };
                    rec->created_lights.push_back(reply.handle);
                }
            }
            lux::render::replyToCurrent<lux::render::CreateLightPayload>(ctx, reply);
        }

        inline void recUpdateLight(Ctx& ctx, const lux::render::UpdateLightPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("UpdateLight", &p, sizeof(p));
        }

        inline void recDestroyLight(Ctx& ctx, const lux::render::DestroyLightPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("DestroyLight", &p, sizeof(p));
            rec->destroyed_lights.push_back(p.handle);
        }

        // ── Mesh-stack handlers (InstanceBridge) ──
        inline void recUploadMesh(Ctx& ctx, const lux::render::UploadMeshPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("UploadMesh", &p, sizeof(p));
            lux::render::MeshUploadedReply reply{};
            if (rec->mesh_upload_null_handle)
            {
                reply.status = 0;   // dispatch-failure default reply: status 0 + null handle
            }
            else
            {
                reply.status = rec->mesh_upload_status;
                if (reply.status == 0)
                    reply.handle = lux::render::RMeshHandle{ rec->next_mesh_index++, 1u };
            }
            lux::render::replyToCurrent<lux::render::UploadMeshPayload>(ctx, reply);
        }

        inline void recAddMeshInstance(Ctx& ctx, const lux::render::AddMeshInstancePayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("AddMeshInstance", &p, sizeof(p));
            lux::render::MeshInstanceSlotReply reply{};
            reply.status = rec->add_instance_status;
            if (reply.status == lux::render::MeshInstanceCreateStatus::Ok
                || rec->add_instance_object_on_failure)
            {
                reply.object = lux::render::RenderObjectHandle{ rec->next_object_index++, 1u };
                rec->created_objects.push_back(reply.object);
            }
            lux::render::replyToCurrent<lux::render::AddMeshInstancePayload>(ctx, reply);
        }

        inline void recRemoveMeshInstance(Ctx& ctx, const lux::render::RemoveMeshInstancePayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("RemoveMeshInstance", &p, sizeof(p));
        }

        // ── Skybox handler (PARAM) ──
        inline void recSkyboxSetEquirect(Ctx& ctx, const lux::render::SkyboxSetEquirectPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("SkyboxSetEquirect", &p, sizeof(p));
        }

        // Bulk op: one command carries a span of TransformWriteEntry. Record the whole
        // span as one "TransformBatch" blob (each updateTransform emits a bulk-of-1, so
        // a test decodes payload<TransformWriteEntry>(i) as that command's single entry).
        inline void recTransformBatch(Ctx& ctx, std::span<const lux::render::TransformWriteEntry> entries)
        {
            static_cast<Recorder*>(ctx.user_state)->record(
                "TransformBatch", entries.data(), entries.size_bytes());
        }

        // ── Canvas2D v2 instance handlers (Sprite2DBridge) ──
        inline void recAddSprite2D(Ctx& ctx, const lux::render::AddSprite2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("AddSprite2D", &p, sizeof(p));
            lux::render::Sprite2DSlotReply reply{};
            reply.status = rec->add_sprite_status;
            if (reply.status == lux::render::ECanvas2DCreateStatus::Ok)
            {
                reply.handle = lux::render::Sprite2DHandle{ rec->next_sprite_index++, 1u };
                rec->created_sprites.push_back(reply.handle);
            }
            lux::render::replyToCurrent<lux::render::AddSprite2DPayload>(ctx, reply);
        }

        inline void recRemoveSprite2D(Ctx& ctx, const lux::render::RemoveSprite2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("RemoveSprite2D", &p, sizeof(p));
            rec->removed_sprites.push_back(p.handle);
        }

        // Bulk: one command carries a span of dirty transform entries. Recorded as one
        // "Sprite2DTransformBatch" blob (decode as Sprite2DTransformEntry[]).
        inline void recSprite2DTransformBatch(Ctx& ctx, std::span<const lux::render::Sprite2DTransformEntry> entries)
        {
            static_cast<Recorder*>(ctx.user_state)->record(
                "Sprite2DTransformBatch", entries.data(), entries.size_bytes());
        }

        inline void recUpdateSprite2DVisual(Ctx& ctx, const lux::render::UpdateSprite2DVisualPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("UpdateSprite2DVisual", &p, sizeof(p));
        }

        inline void recUpdateSprite2DKey(Ctx& ctx, const lux::render::UpdateSprite2DKeyPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("UpdateSprite2DKey", &p, sizeof(p));
        }

        inline void recSetCanvas2DEnabled(Ctx& ctx, const lux::render::SetCanvas2DEnabledPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("SetCanvas2DEnabled", &p, sizeof(p));
        }
    } // namespace detail

    // ── The fixture: client session + generic fake server + recording dispatcher ──
    class HeadlessBridgeFixture
    {
    public:
        using Channel    = lux::render::RenderProgramChannel<>;
        using Dispatcher = lux::render::FrameDispatcher<>;
        using Server     = lux::render::RenderServer<>;

        HeadlessBridgeFixture()
            : channel_(Channel::create())
            , sync_(std::make_shared<lux::render::RenderChannelSync>())
            , session_(channel_, sync_)
            , server_(channel_, sync_, dispatcher_)
        {
        }

        // Register recording handlers for the Light feature's CRUD ops at freshly
        // allocated dynamic TypeIds, then inject "Light" → those ids into the
        // FeatureRegistry. A PoolBridge<*LightComponent> resolves its ops by name
        // through this registry, so it dispatches to handlers the fake server owns.
        // Order MUST match FeatureOpIds<CreateLightOp, UpdateLightOp, DestroyLightOp,
        // LightBatchOp> (LightBatch is unused by PoolBridge, so left unregistered).
        void registerLightOps()
        {
            using namespace lux::render;
            const TypeId create  = dispatcher_.allocateAndRegisterUnary<CreateLightPayload,  &detail::recCreateLight >(opcode_of_v<CreateLightOp>,  "CreateLight");
            const TypeId update  = dispatcher_.allocateAndRegisterUnary<UpdateLightPayload,  &detail::recUpdateLight >(opcode_of_v<UpdateLightOp>,  "UpdateLight");
            const TypeId destroy = dispatcher_.allocateAndRegisterUnary<DestroyLightPayload, &detail::recDestroyLight>(opcode_of_v<DestroyLightOp>, "DestroyLight");
            const std::array<TypeId, 3> ops{ create, update, destroy };
            features_.injectForTest("Light", ops);
        }

        // Register the mesh-stack ops the InstanceBridge emits and stash the resulting
        // MeshStackOperationIds — the InstanceBridge resolves these via ctx.meshStackOps()
        // (set by RenderableSystem::setMeshStackOps), NOT the FeatureRegistry. Only the
        // ops an instance's create → live → reap path actually hits are wired: UploadMesh
        // (Resource, replies a handle), AddMeshInstance (Stream, replies handle+status),
        // RemoveMeshInstance (Stream). Unwired slots stay kInvalidTypeId — the matching
        // send<Op> no-ops, which is correct for ops this fixture's tests never exercise.
        void registerMeshStackOps()
        {
            using namespace lux::render;
            // FeatureOpIds order: Add, Remove, MakeVisible, HideFromView, UpdateFlags,
            // UpdateRenderState, UpdateUserMeta, TransformBatch, UploadMesh, DestroyMesh.
            std::array<TypeId, 10> ids;
            ids.fill(kInvalidTypeId);
            ids[0] = dispatcher_.allocateAndRegisterUnary<AddMeshInstancePayload,   &detail::recAddMeshInstance  >(opcode_of_v<AddMeshInstanceOp>,   "AddMeshInstance");
            ids[1] = dispatcher_.allocateAndRegisterUnary<RemoveMeshInstancePayload,&detail::recRemoveMeshInstance>(opcode_of_v<RemoveMeshInstanceOp>,"RemoveMeshInstance");
            ids[7] = dispatcher_.allocateAndRegisterBulk <TransformWriteEntry,      &detail::recTransformBatch   >(opcode_of_v<TransformBatchOp>,   "TransformBatch");
            ids[8] = dispatcher_.allocateAndRegisterUnary<UploadMeshPayload,        &detail::recUploadMesh       >(opcode_of_v<UploadMeshOp>,        "UploadMesh");
            mesh_stack_ops_ = MeshStackOperationIds::fromOps(ids.data(), static_cast<std::uint32_t>(ids.size()));
        }

        [[nodiscard]] lux::render::MeshStackOperationIds meshStackOps() const noexcept { return mesh_stack_ops_; }

        // Register the Skybox feature's SetEquirect op (a PARAM feature) + inject
        // "Skybox" into the FeatureRegistry with a VALID feature handle, so a
        // ParamBridge<SkyboxComponent> resolves the feature (drive + the teardown clear
        // both require features().handle("Skybox").valid()).
        void registerSkyboxOps()
        {
            using namespace lux::render;
            std::array<TypeId, 2> ids;   // FeatureOpIds order: SetEquirect, SetCubemap
            ids.fill(kInvalidTypeId);
            ids[0] = dispatcher_.allocateAndRegisterUnary<SkyboxSetEquirectPayload, &detail::recSkyboxSetEquirect>(opcode_of_v<SkyboxSetEquirectOp>, "SkyboxSetEquirect");
            features_.injectForTest("Skybox", ids, FeatureHandle{ 0u, 1u });
        }

        // Register the Canvas2D v2 instance ops + inject "Canvas2D" → their ids, so a
        // retained Sprite2DBridge / Camera2DUploadBridge resolving ctx.canvas2d()
        // (features().ops<Canvas2DOperationIds>) dispatches to handlers the fake server
        // owns. Order matches FeatureOpIds<AddSprite2DOp, RemoveSprite2DOp,
        // Sprite2DTransformBatchOp, UpdateSprite2DVisualOp, UpdateSprite2DKeyOp,
        // SetCanvas2DEnabledOp>.
        void registerCanvas2DOps()
        {
            using namespace lux::render;
            std::array<TypeId, 6> ids;
            ids[0] = dispatcher_.allocateAndRegisterUnary<AddSprite2DPayload,        &detail::recAddSprite2D        >(opcode_of_v<AddSprite2DOp>,            "AddSprite2D");
            ids[1] = dispatcher_.allocateAndRegisterUnary<RemoveSprite2DPayload,     &detail::recRemoveSprite2D     >(opcode_of_v<RemoveSprite2DOp>,         "RemoveSprite2D");
            ids[2] = dispatcher_.allocateAndRegisterBulk <Sprite2DTransformEntry,    &detail::recSprite2DTransformBatch>(opcode_of_v<Sprite2DTransformBatchOp>, "Sprite2DTransformBatch");
            ids[3] = dispatcher_.allocateAndRegisterUnary<UpdateSprite2DVisualPayload,&detail::recUpdateSprite2DVisual>(opcode_of_v<UpdateSprite2DVisualOp>,  "UpdateSprite2DVisual");
            ids[4] = dispatcher_.allocateAndRegisterUnary<UpdateSprite2DKeyPayload,  &detail::recUpdateSprite2DKey  >(opcode_of_v<UpdateSprite2DKeyOp>,      "UpdateSprite2DKey");
            ids[5] = dispatcher_.allocateAndRegisterUnary<SetCanvas2DEnabledPayload, &detail::recSetCanvas2DEnabled >(opcode_of_v<SetCanvas2DEnabledOp>,     "SetCanvas2DEnabled");
            features_.injectForTest("Canvas2D", ids);
        }

        // ── Granular frame steps (for tests that must interleave shutdown between
        //    a command and its reply, e.g. the P0 drain). ──
        void beginFrame() { session_.beginFrame({}); }
        void submit()     { session_.submitFrame(/*blocking=*/false); }
        void dispatch()   { server_.drainAndDispatch(&recorder_); }   // fake server processes + queues replies
        void pump()       { session_.pumpReplies(); }                 // client fires .then continuations

        // One synchronous client↔fake-server round-trip. Mirrors the RenderableSystem
        // tick order: submit the begun frame, let the fake server dispatch + reply,
        // pump the client continuations (builder NOT live here — matches the real
        // constraint that .then callbacks may only touch local maps), reopen a frame.
        void roundTrip()
        {
            submit();
            dispatch();
            pump();
            beginFrame();
        }

        [[nodiscard]] lux::render::RenderSession&   session()  noexcept { return session_; }
        [[nodiscard]] lux::render::FeatureRegistry& features() noexcept { return features_; }
        [[nodiscard]] lux::asset::AssetManager&     assetMgr() noexcept { return asset_mgr_; }
        [[nodiscard]] Dispatcher&                   dispatcher() noexcept { return dispatcher_; }
        [[nodiscard]] Recorder&                     recorder() noexcept { return recorder_; }

        // Fake, non-null scene/view handles. No real scene is created (that needs a
        // GPU) — the bridge only carries these through into command payloads, which
        // the recording handlers do not validate.
        [[nodiscard]] lux::render::RenderSceneId scene() const noexcept { return lux::render::RenderSceneId{ 0u, 1u }; }
        [[nodiscard]] lux::render::ViewHandle    view()  const noexcept { return lux::render::ViewHandle{ 0u, 1u }; }

    private:
        std::shared_ptr<Channel>                        channel_;
        std::shared_ptr<lux::render::RenderChannelSync> sync_;
        lux::render::RenderSession                      session_;
        Dispatcher                                      dispatcher_;
        Server                                          server_;   // holds dispatcher_ by ref → declared after it
        lux::render::FeatureRegistry                    features_;
        lux::asset::AssetManager                        asset_mgr_;
        lux::render::MeshStackOperationIds              mesh_stack_ops_;
        Recorder                                        recorder_;
    };

} // namespace lux::bridgetest
