#pragma once
// ============================================================================
//  HeadlessBridgeFixture.hpp — GPU-free in-process render harness for the
//  ECS→render bridge regression tests (Gate -1 / G-09).
//
//  子系统(PooledSlotSubsystem / MeshInstanceSubsystem / FeatureParamSubsystem)talk to the renderer
//  ONLY through a RenderFrameSession command channel + a FeatureCatalog op-id table.
//  Neither needs a GPU:
//    * RenderFrameSession is a pure command builder + SPSC ring I/O (no Vulkan).
//    * The generic RenderServer<> drains that ring and dispatches through a
//      FrameDispatcher — the dispatcher is a plain vtable, GPU only lives inside
//      the real feature handlers.
//  So we stand up a real client RenderFrameSession against a real (generic)
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

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>      // FrameDispatcher, RenderServer<>, ExecuteContext, replyToCurrent
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/FrameProgram.hpp>             // RenderFrameChannel
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>          // RenderChannelSync, FrameMemoryHints, opcodes
#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>  // opcode_of_v
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>  // MeshStack ops/payloads/proxy ids
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>       // Skybox ops/payloads (PARAM)
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>  // Canvas2D v2 instance ops (2D bridges)
#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>   // Image2DInstanceData / Image2DHandle
#include <lux/engine/function/render/client/RenderProtocol.hpp>          // MeshUploadedReply
#include <lux/engine/function/render/client/resources/ops/TextureResourceOperation.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>       // RenderObjectHandle

#include <lux/engine/resource/asset/AssetManager.hpp>

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

        // Mesh-stack reply controls (MeshInstanceSubsystem).
        std::uint32_t mesh_upload_status{0};    ///< 0 = Ok; non-zero = upload failed
        std::uint32_t next_mesh_index{1};       ///< hand out distinct RMeshHandle indices
        /// Simulate a generic dispatch failure for uploadMesh: reply {status 0, NULL
        /// handle}. Tests P1-4 — status-only validation settles a ready-but-null entry
        /// that re-uploads every frame.
        bool          mesh_upload_null_handle{false};
        std::uint32_t next_object_index{1};     ///< hand out distinct RenderObjectHandle indices
        lux::render::MeshInstanceCreateStatus add_instance_status{
            lux::render::MeshInstanceCreateStatus::Ok};   ///< addMeshInstance outcome the server reports
        lux::render::RenderError add_instance_dispatch_error{};
        /// When true, hand back a NON-null object even on a non-Ok status. This is a
        /// SYNTHETIC shape (the real dispatch-failure wire shape is {Unknown, NULL object}
        /// — RenderRequest delivers a default-constructed reply on CommandFailedReply). It
        /// is forced only to DECOUPLE the MeshInstanceSubsystem's `status != Ok || !object` guard
        /// halves, so a P1-2 test can prove the STATUS check ALONE rejects a would-be-live
        /// reply (otherwise the `!object` half would mask the status check).
        bool add_instance_object_on_failure{false};
        std::vector<lux::render::RenderObjectHandle> created_objects;

        // Canvas2D v2 reply controls (Image2DSubsystem).
        lux::render::ECanvas2DCreateStatus add_image_status{
            lux::render::ECanvas2DCreateStatus::Ok};   ///< AddImage2D outcome the server reports
        lux::render::RenderError add_image_dispatch_error{};
        std::uint32_t next_image_index{1};            ///< hand out distinct Image2DHandle indices
        std::vector<lux::render::Image2DHandle> created_images;
        std::vector<lux::render::Image2DHandle> removed_images;

        // Canvas2D pixel-field reply controls (PixelField2DSubsystem).
        lux::render::ECanvas2DCreateStatus add_pixel_status{
            lux::render::ECanvas2DCreateStatus::Ok};
        std::uint32_t next_pixel_index{1};
        std::vector<lux::render::PixelFieldInstanceHandle> created_pixels;
        std::vector<lux::render::PixelFieldInstanceHandle> removed_pixels;

        // Canvas2D tile reply controls (Tilemap2DSubsystem).
        lux::render::ECanvas2DCreateStatus add_tile_status{
            lux::render::ECanvas2DCreateStatus::Ok};
        lux::render::RenderError add_tile_dispatch_error{};
        std::uint32_t next_tile_index{1};
        std::vector<lux::render::Tile2DInstanceHandle> created_tiles;
        std::vector<lux::render::Tile2DInstanceHandle> removed_tiles;

        // Persistent texture controls (Tilemap2DSubsystem / PixelField2DSubsystem).
        std::uint32_t persistent_texture_create_status{0};
        std::uint32_t texture_region_update_status{0};
        std::uint32_t next_texture_index{1};
        std::vector<lux::render::RTextureHandle> created_textures;
        std::vector<lux::render::RTextureHandle> destroyed_textures;

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

        // ── Mesh-stack handlers (MeshInstanceSubsystem) ──
        /// `destroyMesh` 是 stream op(无回复),记下来就行 —— 它是「引用归零之后
        /// GPU 网格真的被销毁了」的唯一凭据。
        inline void recDestroyMesh(Ctx& ctx, const lux::render::DestroyMeshPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("DestroyMesh", &p, sizeof(p));
        }

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
            if (!rec->add_instance_dispatch_error.ok())
            {
                ctx.markDispatchError(rec->add_instance_dispatch_error);
                return;
            }
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

        inline void recUpdateInstanceFlags(Ctx& ctx, const lux::render::UpdateInstanceFlagsPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("UpdateInstanceFlags", &p, sizeof(p));
        }

        inline void recRemoveMeshInstance(Ctx& ctx, const lux::render::RemoveMeshInstancePayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("RemoveMeshInstance", &p, sizeof(p));
        }

        inline void recRetireMeshInstance(Ctx& ctx, const lux::render::RetireMeshInstancePayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record(
                "RetireMeshInstance", &p, sizeof(p));
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

        // ── Canvas2D v2 instance handlers (Image2DSubsystem) ──
        inline void recAddImage2D(Ctx& ctx, const lux::render::AddImage2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("AddImage2D", &p, sizeof(p));
            if (!rec->add_image_dispatch_error.ok())
            {
                ctx.markDispatchError(rec->add_image_dispatch_error);
                return;
            }
            lux::render::Image2DSlotReply reply{};
            reply.status = rec->add_image_status;
            if (reply.status == lux::render::ECanvas2DCreateStatus::Ok)
            {
                reply.handle = lux::render::Image2DHandle{ rec->next_image_index++, 1u };
                rec->created_images.push_back(reply.handle);
            }
            lux::render::replyToCurrent<lux::render::AddImage2DPayload>(ctx, reply);
        }

        inline void recRemoveImage2D(Ctx& ctx, const lux::render::RemoveImage2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("RemoveImage2D", &p, sizeof(p));
            rec->removed_images.push_back(p.handle);
        }

        // Bulk: one command carries a span of dirty transform entries. Recorded as one
        // "Image2DTransformBatch" blob (decode as Image2DTransformEntry[]).
        inline void recImage2DTransformBatch(Ctx& ctx, std::span<const lux::render::Image2DTransformEntry> entries)
        {
            static_cast<Recorder*>(ctx.user_state)->record(
                "Image2DTransformBatch", entries.data(), entries.size_bytes());
        }

        inline void recUpdateImage2DVisual(Ctx& ctx, const lux::render::UpdateImage2DVisualPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("UpdateImage2DVisual", &p, sizeof(p));
        }

        inline void recUpdateImage2DKey(Ctx& ctx, const lux::render::UpdateImage2DKeyPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("UpdateImage2DKey", &p, sizeof(p));
        }

        inline void recSetCanvas2DEnabled(Ctx& ctx, const lux::render::SetCanvas2DEnabledPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record("SetCanvas2DEnabled", &p, sizeof(p));
        }

        inline void recAddPixelField2D(
            Ctx& ctx,
            const lux::render::AddPixelField2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("AddPixelField2D", &p, sizeof(p));
            lux::render::PixelFieldSlotReply reply{};
            reply.status = rec->add_pixel_status;
            if (reply.status == lux::render::ECanvas2DCreateStatus::Ok)
            {
                reply.handle = lux::render::PixelFieldInstanceHandle{
                    rec->next_pixel_index++,
                    1u
                };
                rec->created_pixels.push_back(reply.handle);
            }
            lux::render::replyToCurrent<
                lux::render::AddPixelField2DPayload>(ctx, reply);
        }

        inline void recRemovePixelField2D(
            Ctx& ctx,
            const lux::render::RemovePixelField2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("RemovePixelField2D", &p, sizeof(p));
            rec->removed_pixels.push_back(p.handle);
        }

        inline void recUpdatePixelField2DTransform(
            Ctx& ctx,
            const lux::render::UpdatePixelField2DTransformPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record(
                "UpdatePixelField2DTransform",
                &p,
                sizeof(p)
            );
        }

        inline void recUpdatePixelField2DKey(
            Ctx& ctx,
            const lux::render::UpdatePixelField2DKeyPayload& p)
        {
            static_cast<Recorder*>(ctx.user_state)->record(
                "UpdatePixelField2DKey",
                &p,
                sizeof(p)
            );
        }

        inline void recAddTile2D(Ctx& ctx, const lux::render::AddTile2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("AddTile2D", &p, sizeof(p));
            if (!rec->add_tile_dispatch_error.ok())
            {
                ctx.markDispatchError(rec->add_tile_dispatch_error);
                return;
            }
            lux::render::Tile2DSlotReply reply{};
            reply.status = rec->add_tile_status;
            if (reply.status == lux::render::ECanvas2DCreateStatus::Ok)
            {
                reply.handle = lux::render::Tile2DInstanceHandle{
                    rec->next_tile_index++, 1u};
                rec->created_tiles.push_back(reply.handle);
            }
            lux::render::replyToCurrent<lux::render::AddTile2DPayload>(ctx, reply);
        }

        inline void recRemoveTile2D(Ctx& ctx, const lux::render::RemoveTile2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("RemoveTile2D", &p, sizeof(p));
            rec->removed_tiles.push_back(p.handle);
        }

        // ── Persistent texture resource handlers (Tilemap2DSubsystem) ──
        inline void recCreatePersistentTexture2D(
            Ctx& ctx,
            const lux::render::CreatePersistentTexture2DPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("CreatePersistentTexture2D", &p, sizeof(p));
            lux::render::Texture2DCreatedReply reply{};
            reply.status = rec->persistent_texture_create_status;
            if (reply.status == 0)
            {
                reply.handle = lux::render::RTextureHandle{
                    rec->next_texture_index++, 1u};
                rec->created_textures.push_back(reply.handle);
            }
            lux::render::replyToCurrent<
                lux::render::CreatePersistentTexture2DPayload>(ctx, reply);
        }

        inline void recUpdateTextureRegions(
            Ctx& ctx,
            const lux::render::UpdateTextureRegionsPayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("UpdateTextureRegions", &p, sizeof(p));
            lux::render::replyToCurrent<lux::render::UpdateTextureRegionsPayload>(
                ctx,
                lux::render::TextureRegionsAppliedReply{
                    p.content_revision,
                    rec->texture_region_update_status,
                    0u
                }
            );
        }

        inline void recDestroyTexture(
            Ctx& ctx,
            const lux::render::DestroyTexturePayload& p)
        {
            auto* rec = static_cast<Recorder*>(ctx.user_state);
            rec->record("DestroyTexture", &p, sizeof(p));
            rec->destroyed_textures.push_back(p.handle);
        }
    } // namespace detail

    // ── The fixture: client session + generic fake server + recording dispatcher ──
    class HeadlessBridgeFixture
    {
    public:
        using Channel    = lux::render::RenderFrameChannel<>;
        using ControlChannel = lux::render::RenderControlChannel<>;
        using UploadChannel = lux::render::RenderUploadChannel<>;
        using Dispatcher = lux::render::FrameDispatcher<>;
        using Server     = lux::render::RenderServer<>;
        using ControlServer = lux::render::RenderControlServer;

        HeadlessBridgeFixture()
            : asset_mgr_(lux::asset::runtimeAssetCodecCatalog())
            , channel_(Channel::create())
            , control_channel_(ControlChannel::create())
            , upload_channel_(UploadChannel::create())
            , sync_(std::make_shared<lux::render::RenderChannelSync>())
            , session_(channel_, sync_)
            , control_(control_channel_, sync_)
            , upload_(upload_channel_, sync_)
            , server_(channel_, sync_, dispatcher_)
            , control_server_(control_channel_, sync_, dispatcher_)
            , upload_server_(upload_channel_, sync_, dispatcher_)
        {
            direct_upload_state_ = std::make_shared<DirectUploadState>();
            direct_upload_state_->session = &upload_;
        }

        ~HeadlessBridgeFixture()
        {
            direct_upload_state_->session = nullptr;
        }

        // Register recording handlers for the Light feature's CRUD ops at freshly
        // allocated dynamic TypeIds, then inject "Light" → those ids into the
        // FeatureCatalog. A pooled-slot light subsystem resolves its ops by name
        // through this registry, so it dispatches to handlers the fake server owns.
        //
        // ⚠️ Slot order MUST match the GENERATED `LightOperationIds`, which is
        //    FeatureOpIds<CreateLightOp, UpdateLightOp, LightBatchOp,
        //    DestroyLightOp, LightStatsOp> — LightBatch is slot 2, Destroy is
        //    slot 3 and Stats is slot 4. This comment used to claim
        //    Destroy/LightBatch the other way round and injected a 3-element array,
        //    so `destroy` landed in the BATCH slot and the real Destroy slot stayed
        //    kInvalidTypeId: every destroyLight() this fixture ever drove was a
        //    silent no-op, and no test could see it (the light-bridge tests that
        //    would have are development-time artifacts, deleted long ago). Read the
        //    order off comm_gen/…/LightOperation.ops.hpp, not off a comment.
        //    LightBatch is unused by PooledSlotSubsystem, so its slot stays invalid.
        void registerLightOps()
        {
            using namespace lux::render;
            const TypeId create  = dispatcher_.allocateAndRegisterUnary<CreateLightPayload,  &detail::recCreateLight >(opcode_of_v<CreateLightOp>,  "CreateLight");
            const TypeId update  = dispatcher_.allocateAndRegisterUnary<UpdateLightPayload,  &detail::recUpdateLight >(opcode_of_v<UpdateLightOp>,  "UpdateLight");
            const TypeId destroy = dispatcher_.allocateAndRegisterUnary<DestroyLightPayload, &detail::recDestroyLight>(opcode_of_v<DestroyLightOp>, "DestroyLight");
            const std::array<TypeId, 5> ops{
                create,
                update,
                kInvalidTypeId /*LightBatch*/,
                destroy,
                kInvalidTypeId /*LightStats*/};
            features_.injectForTest("Light", ops);
        }

        // Register the mesh-stack ops the MeshInstanceSubsystem emits, injected under
        // "StandardMeshStack" into the FeatureCatalog — the MeshInstanceSubsystem and the
        // 驻留子服务 both resolve them BY NAME through this single catalogue.
        // Only the ops an instance's
        // create → live → leave path actually hits are wired: UploadMesh (Resource,
        // replies a handle), AddMeshInstance (Stream, replies handle+status),
        // RemoveMeshInstance (Stream). Unwired slots stay kInvalidTypeId — the matching
        // send<Op> no-ops, which is correct for ops this fixture's tests never exercise.
        void registerMeshStackOps()
        {
            using namespace lux::render;
            // FeatureOpIds order is generated declaration order. Keep the fake
            // catalogue aligned so World Actor teardown reaches Retire rather
            // than silently dispatching a neighbouring operation.
            // Add, Remove, Retire, Stats, MakeVisible, HideFromView,
            // UpdateFlags, UpdateRenderState, UpdateUserMeta, TransformBatch,
            // UploadMesh, DestroyMesh.
            std::array<TypeId, 12> ids;
            ids.fill(kInvalidTypeId);
            ids[0] = dispatcher_.allocateAndRegisterUnary<AddMeshInstancePayload,   &detail::recAddMeshInstance  >(opcode_of_v<AddMeshInstanceOp>,   "AddMeshInstance");
            ids[1] = dispatcher_.allocateAndRegisterUnary<RemoveMeshInstancePayload,&detail::recRemoveMeshInstance>(opcode_of_v<RemoveMeshInstanceOp>,"RemoveMeshInstance");
            ids[2] = dispatcher_.allocateAndRegisterUnary<RetireMeshInstancePayload,&detail::recRetireMeshInstance>(opcode_of_v<RetireMeshInstanceOp>,"RetireMeshInstance");
            // 高亮改成实体标签之后(阶段 5),「标志位有没有被推上去」只有这条能证。
            ids[6] = dispatcher_.allocateAndRegisterUnary<UpdateInstanceFlagsPayload,&detail::recUpdateInstanceFlags>(opcode_of_v<UpdateInstanceFlagsOp>,"UpdateInstanceFlags");
            ids[9] = dispatcher_.allocateAndRegisterBulk <TransformWriteEntry,      &detail::recTransformBatch   >(opcode_of_v<TransformBatchOp>,   "TransformBatch");
            ids[10] = dispatcher_.allocateAndRegisterUnary<UploadMeshPayload,       &detail::recUploadMesh       >(opcode_of_v<UploadMeshOp>,        "UploadMesh");
            // 引用计数搬去 AssetManager 之后,「最后一个组件松手了,GPU 网格有没有被
            // 销毁」只有这条能证 —— 漏了就是静默泄漏(退出码 0、日志一行不差)。
            ids[11] = dispatcher_.allocateAndRegisterUnary<DestroyMeshPayload,      &detail::recDestroyMesh      >(opcode_of_v<DestroyMeshOp>,       "DestroyMesh");
            features_.injectForTest("StandardMeshStack", ids);
        }

        // Register the Skybox feature's SetEquirect op (a PARAM feature) + inject
        // "Skybox" into the FeatureCatalog. 句柄是场景域状态 —— 需要它的测试在
        // 自己的 RenderSystem 上 `rs.bindFeature("Skybox", {0,1})`(drive 与
        // teardown clear 都要求 features().handle("Skybox").valid())。
        void registerSkyboxOps()
        {
            using namespace lux::render;
            std::array<TypeId, 2> ids;   // FeatureOpIds order: SetEquirect, SetCubemap
            ids.fill(kInvalidTypeId);
            ids[0] = dispatcher_.allocateAndRegisterUnary<SkyboxSetEquirectPayload, &detail::recSkyboxSetEquirect>(opcode_of_v<SkyboxSetEquirectOp>, "SkyboxSetEquirect");
            features_.injectForTest("Skybox", ids);
        }

        // Register the Canvas2D v2 instance ops + inject "Canvas2D" → their ids, so a
        // retained Image2DSubsystem / Camera2DUploadSubsystem resolving ctx.canvas2d()
        // (features().ops<Canvas2DOperationIds>) dispatches to handlers the fake server
        // owns. The generated operation list also contains PixelField and Tile
        // operations; unused slots remain invalid, while Tilemap lifecycle tests
        // wire AddTile2D / RemoveTile2D explicitly.
        void registerCanvas2DOps()
        {
            using namespace lux::render;
            // FeatureOpIds declaration order:
            //   Image[0..5], PixelField[6..9], Tile[10..13].
            std::array<TypeId, 14> ids;
            ids.fill(kInvalidTypeId);
            ids[0] = dispatcher_.allocateAndRegisterUnary<AddImage2DPayload,        &detail::recAddImage2D        >(opcode_of_v<AddImage2DOp>,            "AddImage2D");
            ids[1] = dispatcher_.allocateAndRegisterUnary<RemoveImage2DPayload,     &detail::recRemoveImage2D     >(opcode_of_v<RemoveImage2DOp>,         "RemoveImage2D");
            ids[2] = dispatcher_.allocateAndRegisterBulk <Image2DTransformEntry,    &detail::recImage2DTransformBatch>(opcode_of_v<Image2DTransformBatchOp>, "Image2DTransformBatch");
            ids[3] = dispatcher_.allocateAndRegisterUnary<UpdateImage2DVisualPayload,&detail::recUpdateImage2DVisual>(opcode_of_v<UpdateImage2DVisualOp>,  "UpdateImage2DVisual");
            ids[4] = dispatcher_.allocateAndRegisterUnary<UpdateImage2DKeyPayload,  &detail::recUpdateImage2DKey  >(opcode_of_v<UpdateImage2DKeyOp>,      "UpdateImage2DKey");
            ids[5] = dispatcher_.allocateAndRegisterUnary<SetCanvas2DEnabledPayload, &detail::recSetCanvas2DEnabled >(opcode_of_v<SetCanvas2DEnabledOp>,     "SetCanvas2DEnabled");
            ids[6] = dispatcher_.allocateAndRegisterUnary<AddPixelField2DPayload,   &detail::recAddPixelField2D    >(opcode_of_v<AddPixelField2DOp>,       "AddPixelField2D");
            ids[7] = dispatcher_.allocateAndRegisterUnary<RemovePixelField2DPayload,&detail::recRemovePixelField2D >(opcode_of_v<RemovePixelField2DOp>,    "RemovePixelField2D");
            ids[8] = dispatcher_.allocateAndRegisterUnary<UpdatePixelField2DTransformPayload, &detail::recUpdatePixelField2DTransform>(opcode_of_v<UpdatePixelField2DTransformOp>, "UpdatePixelField2DTransform");
            ids[9] = dispatcher_.allocateAndRegisterUnary<UpdatePixelField2DKeyPayload, &detail::recUpdatePixelField2DKey>(opcode_of_v<UpdatePixelField2DKeyOp>, "UpdatePixelField2DKey");
            ids[10] = dispatcher_.allocateAndRegisterUnary<AddTile2DPayload,        &detail::recAddTile2D          >(opcode_of_v<AddTile2DOp>,             "AddTile2D");
            ids[11] = dispatcher_.allocateAndRegisterUnary<RemoveTile2DPayload,     &detail::recRemoveTile2D       >(opcode_of_v<RemoveTile2DOp>,          "RemoveTile2D");
            features_.injectForTest("Canvas2D", ids);
        }

        /// Register the fixed ResourceOp ids used by persistent bridge-owned
        /// textures. These are protocol ids (not dynamic feature ids), so the
        /// fake dispatcher mirrors RenderServerHandlers registration exactly.
        void registerPersistentTextureOps()
        {
            using namespace lux::render;
            dispatcher_.registerUnary<
                CreatePersistentTexture2DPayload,
                &detail::recCreatePersistentTexture2D>(
                    opcodes::ResourceOp,
                    type_ids::CreatePersistentTexture2D,
                    "CreatePersistentTexture2D"
                );
            dispatcher_.registerUnary<
                UpdateTextureRegionsPayload,
                &detail::recUpdateTextureRegions>(
                    opcodes::ResourceOp,
                    type_ids::UpdateTextureRegions,
                    "UpdateTextureRegions"
                );
            dispatcher_.registerUnary<
                DestroyTexturePayload,
                &detail::recDestroyTexture>(
                    opcodes::ResourceOp,
                    type_ids::DestroyTexture,
                    "DestroyTexture"
                );
        }

        // ── Granular frame steps (for tests that must interleave shutdown between
        //    a command and its reply, e.g. the P0 drain). ──
        void beginFrame() { (void)session_.beginFrame({}); }
        void submit()     { (void)session_.trySubmitFrame(); }
        void dispatch()
        {
            while (upload_server_.drainAndDispatch(&recorder_)) {}
            while (control_server_.drainAndDispatch(&recorder_)) {}
            server_.drainAndDispatch(&recorder_);
        }
        void pump()
        {
            if (!upload_.coordinatorOwned())
                upload_.pumpReplies();
            control_.pumpReplies();
            session_.pumpReplies();
        }

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

        [[nodiscard]] lux::render::RenderFrameSession&   session()  noexcept { return session_; }
        [[nodiscard]] lux::render::RenderControlSession& control() noexcept
        {
            return control_;
        }
        [[nodiscard]] lux::render::RenderUploadSession& upload() noexcept
        {
            return upload_;
        }
        [[nodiscard]] lux::render::RenderUploadClient
        uploadClientForTest() const noexcept
        {
            return lux::render::RenderUploadClient::bind(
                direct_upload_state_,
                +[](void* opaque,
                    std::shared_ptr<
                        lux::render::detail::PreparedUpload> prepared) noexcept
                    -> lux::render::UploadSubmitNoReplyResult
                {
                    auto* state = static_cast<DirectUploadState*>(opaque);
                    if (!state || !state->session || !prepared)
                    {
                        return lux::cxx::unexpected(
                            lux::render::ERenderUploadSubmitError::STOPPING);
                    }
                    if (prepared->expected_reply_type ==
                        lux::render::kInvalidTypeId)
                    {
                        return state->session->trySubmitPreparedNoReply(
                            prepared->packet);
                    }
                    return state->session->trySubmitPrepared(
                        prepared->packet,
                        prepared->expected_reply_type,
                        std::move(prepared->callback));
                });
        }
        [[nodiscard]] std::shared_ptr<lux::render::RenderChannelSync>
        sync() const noexcept
        {
            return sync_;
        }
        [[nodiscard]] lux::render::FeatureCatalog& features() noexcept { return features_; }
        [[nodiscard]] lux::asset::AssetManager&     assetMgr() noexcept { return asset_mgr_; }
        [[nodiscard]] Dispatcher&                   dispatcher() noexcept { return dispatcher_; }
        [[nodiscard]] Recorder&                     recorder() noexcept { return recorder_; }

        // Fake, non-null scene/view handles. No real scene is created (that needs a
        // GPU) — the bridge only carries these through into command payloads, which
        // the recording handlers do not validate.
        [[nodiscard]] lux::render::RenderSceneId scene() const noexcept { return lux::render::RenderSceneId{ 0u, 1u }; }
        [[nodiscard]] lux::render::ViewHandle    view()  const noexcept { return lux::render::ViewHandle{ 0u, 1u }; }

    private:
        struct DirectUploadState final
        {
            lux::render::RenderUploadSession* session{nullptr};
        };

        std::shared_ptr<Channel>                        channel_;
        std::shared_ptr<ControlChannel>                 control_channel_;
        std::shared_ptr<UploadChannel>                  upload_channel_;
        std::shared_ptr<lux::render::RenderChannelSync> sync_;
        lux::render::RenderFrameSession                      session_;
        lux::render::RenderControlSession               control_;
        lux::render::RenderUploadSession                upload_;
        std::shared_ptr<DirectUploadState>               direct_upload_state_;
        Dispatcher                                      dispatcher_;
        Server                                          server_;   // holds dispatcher_ by ref → declared after it
        ControlServer                                   control_server_;
        lux::render::RenderUploadServer                 upload_server_;
        lux::render::FeatureCatalog                    features_;
        lux::asset::AssetManager                        asset_mgr_;
        Recorder                                        recorder_;
    };

} // namespace lux::bridgetest
