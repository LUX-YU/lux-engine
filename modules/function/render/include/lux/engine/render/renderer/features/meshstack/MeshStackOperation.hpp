#pragma once
// ============================================================================
//  MeshStackOperation.hpp — StandardMeshStackFeature factory + feature-scoped
//  mesh-instance ops + their payloads.
//
//  Mesh INSTANCES live in InstanceResources — a per-scene resource the
//  StandardMeshStackFeature owns (Stage A). The commands that create / mutate
//  those instances (add / remove / per-view visibility / flags / render-state /
//  user-meta + the per-frame transform batch) are a FEATURE domain: their
//  PAYLOADS, ops and the MeshStackProxy live HERE, dispatched by dynamically
//  allocated TypeIds (register_ops_fn — the grid / light pattern). The core
//  RenderProtocol no longer names mesh instances; a 2D / headless / compute-only
//  scene that omits the feature carries none of this vocabulary. (It still pulls
//  the UNIVERSAL kInstanceFlag* bits + EGeometryKind/PassMask from RenderProtocol,
//  which the core cull shader reads for any instanced consumer.)
//
//  AddMeshInstance REPLIES with the new RenderObjectHandle (MeshInstanceSlotReply)
//  yet stays on CommandOp (Stream kind + handler-side replyToCurrent) — exactly
//  the original pushWithReply(CommandOp,...) shape, NOT the ResourceOp ring.
//
//  Stage B (op downloading): the ops + payloads move here; the heavy instance
//  ASSEMBLY (mesh sections / cull meta / vertex-pool resolution) stays in the
//  server, reached via the exported serverAddMeshInstance shim. Relocating that
//  body into the feature is Stage C (the max bundle).
// ============================================================================

#include <lux/engine/render/comm/RenderProtocol.hpp>        // kInstanceFlag* / EGeometryKind / PassMask / handles + CommandTraits + MeshUploadedReply / ReplyMeshUploaded
#include <lux/engine/render/core/VertexLayoutTypes.hpp>     // VertexLayoutId / kInvalidVertexLayoutId
#include <lux/engine/render/resources/ops/ResourceOperationCommon.hpp>  // DestroyResourcePayload
#include <lux/engine/render/renderer/features/FeatureOps.hpp>  // EOpKind / FeatureOpIds / reply_type_id_of_v
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::rdesc { struct Mesh; }   // uploadMesh deep-copies a Mesh descriptor

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Instance command payloads (moved out of core RenderProtocol.hpp — the
    //  core protocol no longer names mesh instances).
    // =========================================================================
    struct AddMeshInstancePayload
    {
        RenderSceneId       scene_id{};
        RMeshHandle         mesh{};
        RMaterialHandle     material{};
        float transform[16]{}; // column-major 4x4
        uint32_t flags{
            kInstanceFlagCastShadow |
            kInstanceFlagReceiveShadow |
            kInstanceFlagVisible
        };
        EGeometryKind       geometry_kind{EGeometryKind::StaticMesh};
        PassMask            pass_mask{kPassMaskOpaqueDefault};
        uint32_t            user_meta_index{~0u};
    };
    static_assert(std::is_trivially_copyable_v<AddMeshInstancePayload>);

    struct RemoveMeshInstancePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveMeshInstancePayload>);

    struct MakeInstanceVisibleForViewPayload
    {
        RenderSceneId      scene_id{};
        ViewHandle         view{};
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<MakeInstanceVisibleForViewPayload>);

    struct HideInstanceFromViewPayload
    {
        RenderSceneId      scene_id{};
        ViewHandle         view{};
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<HideInstanceFromViewPayload>);

    struct UpdateInstanceFlagsPayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        uint32_t           flags{0};
    };
    static_assert(std::is_trivially_copyable_v<UpdateInstanceFlagsPayload>);

    struct UpdateInstanceRenderStatePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        EGeometryKind      geometry_kind{EGeometryKind::StaticMesh};
        PassMask           pass_mask{kPassMaskOpaqueDefault};
    };
    static_assert(std::is_trivially_copyable_v<UpdateInstanceRenderStatePayload>);

    struct UpdateInstanceUserMetaPayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        uint32_t           user_meta_index{~0u};
    };
    static_assert(std::is_trivially_copyable_v<UpdateInstanceUserMetaPayload>);

    /// Per-frame transform batch entry (BulkData). Each entry self-routes by its
    /// own `scene_id` (G-04) — the server no longer applies the batch to the
    /// SetActiveScene scene, so transform batches from different scenes (editor
    /// main + PreviewScene) interleaved on the ring never cross. The bulk mechanism
    /// carries no batch header (pushBulk = TypeId + span), so scene_id rides each
    /// entry; batches are typically single-scene (often one entry per instance).
    struct TransformWriteEntry
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        float transform[16]{}; // column-major 4x4
    };
    static_assert(std::is_trivially_copyable_v<TransformWriteEntry>);

    /// Reply for the create (addMeshInstance) op — the new instance handle.
    struct MeshInstanceSlotReply
    {
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<MeshInstanceSlotReply>);

    // addMeshInstance is the one instance op that REPLIES (with the new
    // RenderObjectHandle). Delivery is by request_id; reply_type_id is derived
    // from the Reply TYPE (no hand-picked number, no cross-feature collision).
    template <>
    struct CommandTraits<AddMeshInstancePayload>
    {
        using Reply = MeshInstanceSlotReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<MeshInstanceSlotReply>;
    };

    // =========================================================================
    //  Mesh DATA upload/destroy payloads (moved out of core resources/ops/
    //  MeshResourceOperation.hpp — the core protocol no longer names mesh data ops).
    //  The heavy ASYNC assembly (allocate + GPU transfer worker) stays in the server,
    //  reached via the exported serverUploadMesh / serverDestroyMesh shims. The upload
    //  REPLY (MeshUploadedReply) stays a CORE protocol reply (RenderProtocol.hpp) — it is
    //  emitted by the SHARED async-upload worker (mesh + texture transfers), so this op
    //  reuses the core ReplyMeshUploaded id (there is no synchronous handler-side reply).
    // =========================================================================
    struct UploadMeshPayload
    {
        VertexLayoutId  layout_id{kInvalidVertexLayoutId};
        ExternalDataRef mesh_desc{};   // shared-owned const rdesc::Mesh (the async worker pins it)
    };
    static_assert(std::is_trivially_copyable_v<UploadMeshPayload>);

    using DestroyMeshPayload = DestroyResourcePayload<RMeshHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyMeshPayload>);

    template <>
    struct CommandTraits<UploadMeshPayload>
    {
        using Reply = MeshUploadedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyMeshUploaded;
    };

    // =========================================================================
    //  Typed op declarations — the single source the client Ids and the server
    //  registrar both derive from. Handlers live in MeshStackOperationHandlers.cpp.
    //  All ride CommandOp (Stream) except the per-frame transform batch (Bulk).
    // =========================================================================
    struct AddMeshInstanceOp
    {
        using Payload = AddMeshInstancePayload;
        // Stream → CommandOp; replies via CommandTraits<AddMeshInstancePayload> +
        // the handler's replyToCurrent (request-id correlated). Same shape as the
        // retired core pushWithReply(CommandOp, AddMeshInstance, ...).
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "AddMeshInstance";
    };
    struct RemoveMeshInstanceOp
    {
        using Payload = RemoveMeshInstancePayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "RemoveMeshInstance";
    };
    struct MakeInstanceVisibleForViewOp
    {
        using Payload = MakeInstanceVisibleForViewPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "MakeInstanceVisibleForView";
    };
    struct HideInstanceFromViewOp
    {
        using Payload = HideInstanceFromViewPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "HideInstanceFromView";
    };
    struct UpdateInstanceFlagsOp
    {
        using Payload = UpdateInstanceFlagsPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateInstanceFlags";
    };
    struct UpdateInstanceRenderStateOp
    {
        using Payload = UpdateInstanceRenderStatePayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateInstanceRenderState";
    };
    struct UpdateInstanceUserMetaOp
    {
        using Payload = UpdateInstanceUserMetaPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "UpdateInstanceUserMeta";
    };
    struct TransformBatchOp
    {
        using Payload = TransformWriteEntry;
        static constexpr EOpKind kind = EOpKind::Bulk;
        static constexpr const char* name = "TransformBatch";
    };
    struct UploadMeshOp
    {
        using Payload = UploadMeshPayload;
        // Resource → ResourceOp ring; replies MeshUploadedReply (deferred by the async
        // upload worker, request-id correlated) — same shape as the retired core UploadMesh.
        static constexpr EOpKind kind = EOpKind::Resource;
        static constexpr const char* name = "UploadMesh";
    };
    struct DestroyMeshOp
    {
        using Payload = DestroyMeshPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr OpCode  opcode = opcodes::ResourceOp;   // resource lifecycle, no reply
        static constexpr const char* name = "DestroyMesh";
    };

    /// Operation IDs returned to the client after RegisterFeatureType. A real
    /// (forward-declarable) struct rather than a `using` alias, because gameplay
    /// headers forward-declare it to stay render-light; otherwise it simply IS a
    /// FeatureOpIds (look ops up by op type via id<Op>()).
    struct MeshStackOperationIds
        : FeatureOpIds<AddMeshInstanceOp, RemoveMeshInstanceOp,
                       MakeInstanceVisibleForViewOp, HideInstanceFromViewOp,
                       UpdateInstanceFlagsOp, UpdateInstanceRenderStateOp,
                       UpdateInstanceUserMetaOp, TransformBatchOp,
                       UploadMeshOp, DestroyMeshOp>
    {
        using Ids = FeatureOpIds<AddMeshInstanceOp, RemoveMeshInstanceOp,
                                 MakeInstanceVisibleForViewOp, HideInstanceFromViewOp,
                                 UpdateInstanceFlagsOp, UpdateInstanceRenderStateOp,
                                 UpdateInstanceUserMetaOp, TransformBatchOp,
                                 UploadMeshOp, DestroyMeshOp>;
        MeshStackOperationIds() = default;
        MeshStackOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static MeshStackOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return MeshStackOperationIds{Ids::fromOps(ops, count)};
        }
    };

    /// StandardMeshStackFeature factory (create + register_ops_fn allocating the
    /// 8 ops above). Adding it to a scene IS the opt-in to standard 3D mesh
    /// rendering — the feature owns the per-scene mesh-stack resources AND the
    /// instance ops. A scene that omits it (2D / headless / compute-only) pays
    /// nothing: no ~96MB mesh arenas, no instance SSBO, no instance vocabulary.
    ///
    /// MUST be added BEFORE the mesh-consuming features — Skinning, ForwardMesh,
    /// DeferredGBuffer, MeshShadow and Highlight cache InstanceResources / read
    /// VertexPoolRegistry at their own attach, so the resources must exist first.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kStandardMeshStackFeatureFactory;

    // =========================================================================
    //  Client-side proxy — MeshStackProxy (sends instance CRUD via dynamic ids).
    //  Replaces the retired RenderSession::addMeshInstance / removeMeshInstance /
    //  makeInstanceVisibleForView / hideInstanceFromView / updateInstanceFlags /
    //  updateInstanceRenderState / updateInstanceUserMeta / updateTransform(s).
    // =========================================================================
    class LUX_FUNCTION_PUBLIC MeshStackProxy
    {
    public:
        MeshStackProxy(RenderSession& session, MeshStackOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Create a mesh instance; replies with its RenderObjectHandle.
        [[nodiscard]] RenderRequest<MeshInstanceSlotReply> addMeshInstance(
            RenderSceneId scene_id,
            RMeshHandle mesh, RMaterialHandle material,
            const float transform[16],
            std::uint32_t flags = kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible,
            EGeometryKind geometry_kind = EGeometryKind::StaticMesh,
            PassMask pass_mask = kPassMaskOpaqueDefault,
            std::uint32_t user_meta_index = ~0u);

        void removeMeshInstance(RenderSceneId scene_id, RenderObjectHandle object);

        /// Make / hide an existing instance in a specific view.
        void makeInstanceVisibleForView(RenderSceneId scene_id, ViewHandle view, RenderObjectHandle object);
        void hideInstanceFromView(RenderSceneId scene_id, ViewHandle view, RenderObjectHandle object);

        void updateInstanceFlags(RenderSceneId scene_id, RenderObjectHandle object, std::uint32_t flags);
        void updateInstanceRenderState(RenderSceneId scene_id, RenderObjectHandle object, EGeometryKind geometry_kind, PassMask pass_mask);
        void updateInstanceUserMeta(RenderSceneId scene_id, RenderObjectHandle object, std::uint32_t user_meta_index);

        /// Per-frame transform updates (batched as bulk data). Each write self-routes
        /// by `scene_id` (G-04), NOT the SetActiveScene scene. `updateTransforms` takes
        /// entries that already carry their own scene_id.
        void updateTransform(RenderSceneId scene_id, RenderObjectHandle object, const float transform[16]);
        void updateTransforms(std::span<const TransformWriteEntry> entries);

        /// Upload mesh DATA into the global mesh arena; replies with the new RMeshHandle.
        /// The Mesh descriptor is deep-COPIED into a shared buffer the async upload worker
        /// keeps alive (the GPU transfer outlives submitFrame) — NOT a borrow.
        [[nodiscard]] RenderRequest<MeshUploadedReply> uploadMesh(
            const lux::rdesc::Mesh& mesh, VertexLayoutId layout_id = kDefaultVertexLayoutId);
        void destroyMesh(RMeshHandle handle);

        [[nodiscard]] bool valid() const noexcept { return ops_.valid(); }

    private:
        RenderSession*        session_;
        MeshStackOperationIds ops_;
    };

} // namespace lux::render
