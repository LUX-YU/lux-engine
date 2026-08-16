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

#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>   // kInstanceFlag* / EGeometryKind / PassMask
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>          // RMeshHandle 等跨线程句柄
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>      // TypeId / CommandTraits 主模板
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>                 // RenderSceneId
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>                 // ViewHandle
#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>     // VertexLayoutId / kInvalidVertexLayoutId
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/render/client/resources/ops/ResourceOperationCommon.hpp>  // DestroyResourcePayload
#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>  // EOpKind / FeatureOpIds / reply_type_id_of_v
#include <lux/engine/function/visibility.h>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::rdesc { struct Mesh; }   // uploadMesh deep-copies a Mesh descriptor

namespace lux::render
{
    struct FeatureFactory;
    class RenderFrameSession;
    class RenderUploadSession;
    enum class ERenderUploadSubmitError : std::uint8_t;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Instance command payloads (moved out of core RenderProtocol.hpp — the
    //  core protocol no longer names mesh instances).
    // =========================================================================
    struct LUX_OP(lane=frame, kind=stream, name=AddMeshInstance, method=addMeshInstance,
                  reply=MeshInstanceSlotReply)
    AddMeshInstancePayload
    {
        RenderSceneId       scene_id{};
        RMeshHandle         mesh{};
        RMaterialHandle     material{};
        RenderSpatialTransform3D transform{};
        uint32_t flags{
            kInstanceFlagCastShadow |
            kInstanceFlagReceiveShadow |
            kInstanceFlagVisible
        };
        EGeometryKind       geometry_kind{EGeometryKind::StaticMesh};
        PassMask            pass_mask{kPassMaskOpaqueDefault};
        uint32_t            user_meta_index{~0u};
        /// Non-zero only for persistent World actors. The render thread starts
        /// the fade on the creation edge so the first visible frame cannot pop.
        std::uint32_t       transition_milliseconds{0u};
        std::uint32_t       transition_seed{0u};
    };
    static_assert(std::is_trivially_copyable_v<AddMeshInstancePayload>);

    struct LUX_OP(lane=frame, kind=stream, name=RemoveMeshInstance, method=removeMeshInstance)
    RemoveMeshInstancePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveMeshInstancePayload>);

    /// Transfers an opaque/masked instance from ECS ownership to the Render
    /// owner for a bounded fade-out. Transparent instances are deliberately
    /// hard-cut by the server. The object handle is generation checked, and
    /// the render owner keeps its Mesh/Material resources alive until expiry.
    struct LUX_OP(lane=frame, kind=stream, name=RetireMeshInstance,
                  method=retireMeshInstance)
    RetireMeshInstancePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        std::uint32_t      transition_milliseconds{350u};
        std::uint32_t      transition_seed{1u};
    };
    static_assert(std::is_trivially_copyable_v<
        RetireMeshInstancePayload>);

    struct MeshStackStatsReply final
    {
        std::uint32_t alive_instances{0u};
        std::uint32_t cluster_owned_instances{0u};
        std::uint32_t transitioning_instances{0u};
        std::uint32_t resource_bound_instances{0u};
        std::uint32_t transparent_hard_cuts{0u};
        std::uint32_t vbo_segment_count{0u};
        std::uint32_t ibo_segment_count{0u};
        std::uint32_t vbo_growth_count{0u};
        std::uint32_t ibo_growth_count{0u};
        std::uint64_t vbo_used_bytes{0u};
        std::uint64_t vbo_free_bytes{0u};
        std::uint64_t vbo_largest_free_block{0u};
        std::uint64_t ibo_used_bytes{0u};
        std::uint64_t ibo_free_bytes{0u};
        std::uint64_t ibo_largest_free_block{0u};
        float vbo_fragmentation{0.0f};
        float ibo_fragmentation{0.0f};
    };
    static_assert(std::is_trivially_copyable_v<MeshStackStatsReply>);

    struct LUX_OP(lane=control, kind=resource, name=MeshStackStats,
                  method=stats, reply=MeshStackStatsReply, opcode=command)
    MeshStackStatsPayload final
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<MeshStackStatsPayload>);

    struct LUX_OP(lane=frame, kind=stream, name=MakeInstanceVisibleForView, method=makeInstanceVisibleForView)
    MakeInstanceVisibleForViewPayload
    {
        RenderSceneId      scene_id{};
        ViewHandle         view{};
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<MakeInstanceVisibleForViewPayload>);

    struct LUX_OP(lane=frame, kind=stream, name=HideInstanceFromView, method=hideInstanceFromView)
    HideInstanceFromViewPayload
    {
        RenderSceneId      scene_id{};
        ViewHandle         view{};
        RenderObjectHandle object{};
    };
    static_assert(std::is_trivially_copyable_v<HideInstanceFromViewPayload>);

    struct LUX_OP(lane=frame, kind=stream, name=UpdateInstanceFlags, method=updateInstanceFlags)
    UpdateInstanceFlagsPayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        uint32_t           flags{0};
    };
    static_assert(std::is_trivially_copyable_v<UpdateInstanceFlagsPayload>);

    struct LUX_OP(lane=frame, kind=stream, name=UpdateInstanceRenderState, method=updateInstanceRenderState)
    UpdateInstanceRenderStatePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        EGeometryKind      geometry_kind{EGeometryKind::StaticMesh};
        PassMask           pass_mask{kPassMaskOpaqueDefault};
    };
    static_assert(std::is_trivially_copyable_v<UpdateInstanceRenderStatePayload>);

    struct LUX_OP(lane=frame, kind=stream, name=UpdateInstanceUserMeta, method=updateInstanceUserMeta)
    UpdateInstanceUserMetaPayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        uint32_t           user_meta_index{~0u};
    };
    static_assert(std::is_trivially_copyable_v<UpdateInstanceUserMetaPayload>);

    /// Per-frame transform batch entry (BulkData). Each entry self-routes by its
    /// own `scene_id` (G-04) — the server no longer applies the batch to the
    /// SetActiveScene scene, so transform batches from different scenes (editor
    /// main + preview worlds) interleaved on the ring never cross. The bulk mechanism
    /// carries no batch header (pushBulk = TypeId + span), so scene_id rides each
    /// entry; batches are typically single-scene (often one entry per instance).
    struct LUX_OP(lane=frame, kind=bulk, name=TransformBatch, method=updateTransforms)
    TransformWriteEntry
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};
        RenderSpatialTransform3D transform{};
    };
    static_assert(std::is_trivially_copyable_v<TransformWriteEntry>);

    /// addMeshInstance outcome. Anything other than Ok ⇒ the instance was NOT created
    /// (object is null); the client must not treat it as live or bump asset refcounts.
    /// `Unknown` is the DEFAULT on purpose: a GENERIC dispatch failure delivers the
    /// continuation a default-constructed reply the server never typed-filled (see
    /// RenderRequest's CommandFailedReply path), so the outcome must NOT default to Ok
    /// (silent zombie) nor to a retriable capacity error (endless retry). Only
    /// CapacityExhausted is transient; Unknown / InvalidConfiguration are permanent.
    enum class MeshInstanceCreateStatus : std::uint32_t
    {
        Unknown              = 0,   // default — generic dispatch/protocol failure (server never set it)
        Ok                   = 1,   // created; object valid
        InvalidConfiguration = 2,   // scene / mesh-stack feature absent — permanent (retry futile until fixed)
        CapacityExhausted    = 3,   // instance / section slot exhausted — transient (may succeed later)
    };

    /// Reply for the create (addMeshInstance) op — the new instance handle + outcome.
    struct MeshInstanceSlotReply
    {
        RenderObjectHandle       object{};
        MeshInstanceCreateStatus status{MeshInstanceCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<MeshInstanceSlotReply>);


    // =========================================================================
    //  Mesh DATA upload/destroy payloads (moved out of core resources/ops/
    //  MeshResourceOperation.hpp — the core protocol no longer names mesh data ops).
    //  The heavy ASYNC assembly (allocate + GPU transfer worker) stays in the server,
    //  reached via the exported serverUploadMesh / serverDestroyMesh shims. The upload
    //  REPLY (MeshUploadedReply) stays a CORE protocol reply (RenderProtocol.hpp) — it is
    //  emitted by the SHARED async-upload worker (mesh + texture transfers), so this op
    //  reuses the core ReplyMeshUploaded id (there is no synchronous handler-side reply).
    // =========================================================================
    struct LUX_OP(lane=upload, kind=resource, name=UploadMesh, method=uploadMesh,
                  reply=MeshUploadedReply, reply_id=type_ids::ReplyMeshUploaded,
                  manual_client=true)
    UploadMeshPayload
    {
        VertexLayoutId  layout_id{kInvalidVertexLayoutId};
        ExternalDataRef mesh_desc{};   // shared-owned const rdesc::Mesh (the async worker pins it)
    };
    static_assert(std::is_trivially_copyable_v<UploadMeshPayload>);

    /// Destroy 载荷:平铺自有结构(原为 DestroyResourcePayload<RMeshHandle> 别名
    /// —— 注解挂不上 using;字段与线上布局逐字节同旧)。
    struct LUX_OP(lane=control, kind=stream, name=DestroyMesh, method=destroyMesh, opcode=resource)
    DestroyMeshPayload
    {
        RMeshHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<DestroyMeshPayload>);

    /// 无客户端创建参数 —— 空 tag 承载特性身份。
    /// requires=material:MaterialResources::init 需要内建 shading-model 表先在
    /// (StandardMaterialFeature attach 时 ensure)——语义必需,但代码里没有
    /// fail-fast 兜底,此前只靠 attach 目录的位置注释撑着。
    struct LUX_COMM_CONFIG(prefix=MeshStack, id=lux.render.mesh_stack.v1, display=StandardMeshStack,
                           requires=lux.render.material.v1,
                           feature=StandardMeshStackFeature,
                           feature_header=lux/engine/render/renderer/features/meshstack/StandardMeshStackFeature.hpp)
    MeshStackCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<MeshStackCommTag>);

    class MeshStackProxy;   // 生成于 comm/genops/MeshStackOperation.ops.hpp
    class MeshStackControlClient;
    class MeshStackUploadClient;

    // ── 便捷面(§7.5,同名自由函数,站点只注接收者;定义在 Handlers.cpp)──
    //    addMeshInstance:多参默认值+矩阵拷贝;uploadMesh:Mesh 深拷进共享缓冲
    //    (ExternalDataRef 非借用);updateTransform(s):单条打包/scene 盖章。
    [[nodiscard]] LUX_FUNCTION_PUBLIC RenderRequest<MeshInstanceSlotReply> addMeshInstance(
        MeshStackProxy proxy, RenderSceneId scene_id,
        RMeshHandle mesh, RMaterialHandle material,
        const RenderSpatialTransform3D& transform,
        std::uint32_t flags = kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible,
        EGeometryKind geometry_kind = EGeometryKind::StaticMesh,
        PassMask pass_mask = kPassMaskOpaqueDefault,
        std::uint32_t user_meta_index = ~0u,
        std::uint32_t transition_milliseconds = 0u,
        std::uint32_t transition_seed = 0u);

    LUX_FUNCTION_PUBLIC void updateTransform(
        MeshStackProxy proxy, RenderSceneId scene_id, RenderObjectHandle object,
        const RenderSpatialTransform3D& transform);

    /// Explicit page-zero adapters for transient preview/test scenes.  They do
    /// not participate in persistent World coordinate conversion.
    [[nodiscard]] LUX_FUNCTION_PUBLIC RenderRequest<MeshInstanceSlotReply>
    addTransientMeshInstance(
        MeshStackProxy proxy, RenderSceneId scene_id,
        RMeshHandle mesh, RMaterialHandle material,
        const float transform[16],
        std::uint32_t flags = kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible,
        EGeometryKind geometry_kind = EGeometryKind::StaticMesh,
        PassMask pass_mask = kPassMaskOpaqueDefault,
        std::uint32_t user_meta_index = ~0u);

    LUX_FUNCTION_PUBLIC void updateTransientMeshTransform(
        MeshStackProxy proxy, RenderSceneId scene_id, RenderObjectHandle object,
        const float transform[16]);

    /// scene 盖章批量(可变 span:server 对 scene_id 为空的条目静默跳过,
    /// 本重载让单场景批次不可能写漏)。
    LUX_FUNCTION_PUBLIC void updateTransforms(
        MeshStackProxy proxy, RenderSceneId scene_id, std::span<TransformWriteEntry> entries);

    /// 逐条自带 scene_id 的异构批次(转发生成面;与盖章重载同名成对)。
    LUX_FUNCTION_PUBLIC void updateTransforms(
        MeshStackProxy proxy, std::span<const TransformWriteEntry> entries);

    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<
        RenderRequest<MeshUploadedReply>, ERenderUploadSubmitError> uploadMesh(
        MeshStackUploadClient client, const lux::rdesc::Mesh& mesh,
        VertexLayoutId layout_id = kDefaultVertexLayoutId);

} // namespace lux::render
