#pragma once
// ============================================================================
//  RenderProtocol.hpp — Core protocol: scene/view/feature lifecycle + replies
//
//  Resource-specific payloads live in resources/ops/*.hpp.
//  Feature-specific payloads live in renderer/features/*/XxxOperation.hpp.
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/PickResult.hpp>
#include <lux/engine/render/core/RenderObjectTypes.hpp>
#include <lux/engine/render/core/RenderResourceHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>

// Resource operation payloads — needed for CommandTraits specializations below
// Mesh data ops moved to a feature: renderer/features/meshstack/MeshStackOperation.hpp
// (StandardMeshStack, dynamic ids via register_ops_fn). Core no longer names mesh data
// ops; the upload REPLY (MeshUploadedReply, below) stays here — it is emitted by the
// shared async-upload worker (mesh + texture), not a mesh op.
#include <lux/engine/render/resources/ops/TextureResourceOperation.hpp>
// Material ops moved to a feature: renderer/features/material/MaterialOperation.hpp
// (StandardMaterial, dynamic ids via register_ops_fn). Core no longer names material.
// The retired ResourceOp ids 8/16/17 (Destroy / UploadGraph / ModifyGraphMaterial) are
// left unused — not reissued — so any stale serialized material command fails closed.
// Light ops moved to a feature: renderer/features/light/LightOperation.hpp
// (LightFeature, dynamic ids via register_ops_fn). Core no longer names light.
#include <lux/engine/render/resources/ops/ShaderResourceOperation.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    // Universal per-instance flags understood by the core renderer. Bits 3+ are
    // RESERVED for RenderFeatures: a feature defines its own flag constant and the
    // gameplay/client side ORs it into the same instance-flags word; the core never
    // decodes those bits (they ride opaquely). Keeping domain flags out of here is
    // what lets a non-editor / 2D / minimal consumer use the protocol without
    // carrying feature-specific semantics. Example feature-owned bit:
    // HighlightOperation.hpp::kInstanceFlagHighlight (bit 3).
    inline constexpr uint32_t kInstanceFlagCastShadow    = 1u << 0;
    inline constexpr uint32_t kInstanceFlagReceiveShadow = 1u << 1;
    inline constexpr uint32_t kInstanceFlagVisible       = 1u << 2;

    // =============================================================================
    //  TypeId constants
    // =============================================================================
    namespace type_ids
    {
        // ---- CommandOp domain ----
        inline constexpr TypeId CreateScene = 1;
        inline constexpr TypeId DestroyScene = 2;
        inline constexpr TypeId AddView = 3;
        inline constexpr TypeId RemoveView = 4;
        inline constexpr TypeId ResizeView = 5;
        inline constexpr TypeId SetSceneTime = 6;
        inline constexpr TypeId Pick = 7;
        // 8/9/10/11/12 were AddMeshInstance / RemoveMeshInstance /
        // Make|HideInstanceForView / UpdateInstanceFlags — mesh instances are now
        // a feature domain (MeshStackOperation.hpp, dynamic ids via register_ops_fn).
        // Left unused (not reissued) so any stale serialized command fails closed.
        inline constexpr TypeId AddFeature = 13;
        inline constexpr TypeId RemoveFeature = 14;
        inline constexpr TypeId SetFeatureEnabled = 15;
        inline constexpr TypeId SetActiveScene = 16;
        inline constexpr TypeId RegisterFeatureType = 17;
        inline constexpr TypeId UnregisterFeatureType = 18;
        // 19/21 were UpdateInstanceRenderState / UpdateInstanceUserMeta — moved to
        // the StandardMeshStack feature (MeshStackOperation.hpp). Left unused.
        inline constexpr TypeId BindSwapchain = 23;
        inline constexpr TypeId RequestSwapchainScene = 24;
        // 25/26 were UploadBonePalette/UploadBoneBatch — skinning is now a feature
        // domain (SkinningOperation.hpp, dynamic ids via register_ops_fn). The core
        // protocol no longer names skinning. Ids 25/26 left unused (not reissued, so
        // any stale serialized command fails closed rather than mis-dispatching).
        inline constexpr TypeId ReadbackView      = 27;  ///< GPU->CPU offscreen-view color readback
        inline constexpr TypeId ReadbackViewAsync = 28;  ///< deferred GPU->CPU readback (reply by request_id)
        inline constexpr TypeId DumpRenderGraph   = 29;  ///< debug: copy the scene's compiled render graph text into a caller buffer (dst_ptr)
        inline constexpr TypeId QueryFeatureParams = 30; ///< enumerate the scene's features + their reflectable params into a caller buffer (dst_ptr)

        // ---- BulkData domain ----
        // 1 was TransformWrite — moved to the StandardMeshStack feature
        // (MeshStackOperation.hpp TransformBatch op). Left unused.
        // 2 was ViewFrameUpdate — per-view camera is the StandardViewCamera feature op
        // now (ViewCameraOperation.hpp, dynamic id). Left unused (not reissued, so any
        // stale serialized command fails closed).
        inline constexpr TypeId SceneTimeUpdate = 3;
        // 4 (LightUpdate) moved to a feature: LightOperation.hpp light batch op
        // (dynamic id via register_ops_fn). Core no longer names light.

        // ---- Reply domain ----
        inline constexpr TypeId ReplySceneCreated = 1;
        inline constexpr TypeId ReplyViewCreated = 2;
        inline constexpr TypeId ReplyPickResult = 3;
        // 4 was ReplyMeshSlot — the addMeshInstance reply is feature-scoped now
        // (MeshStackOperation.hpp; reply_type_id derived from the Reply type).
        inline constexpr TypeId ReplyGenericOk = 5;
        inline constexpr TypeId ReplyFeatureAdded = 6;
        inline constexpr TypeId ReplyFeatureTypeRegistered = 8;
        inline constexpr TypeId ReplyMeshUploaded = 10;
        // 11 was ReplyMaterialUploaded — the upload reply is feature-scoped now
        // (MaterialOperation.hpp; reply_type_id derived from the Reply type).
        inline constexpr TypeId ReplyTexture2DCreated = 12;
        inline constexpr TypeId ReplyCubeTextureCreated = 13;
        // 14 (ReplyLightCreated) moved to LightOperation.hpp (reply tag for the
        // feature-scoped createLight; value reserved). Core no longer names light.
        inline constexpr TypeId ReplyShaderCompiled = 15;
        inline constexpr TypeId ReplyReadbackView   = 20;

        // ---- Name-based TypeId query ----
        inline constexpr TypeId QueryTypeId      = 22;
        inline constexpr TypeId ReplyQueryTypeId = 17;

        // ---- Debug ----
        inline constexpr TypeId ReplyRenderGraphDump = 21;
        inline constexpr TypeId ReplyQueryFeatureParams = 22;  ///< NOTE: reply-id space is separate from the command-id space (QueryTypeId command=22)

        // ---- Swapchain-scene binding ----
        inline constexpr TypeId ReplySwapchainBound = 19;
    } // namespace type_ids

    // =============================================================================
    //  FeatureFactory — function-pointer table for dynamic feature type registration
    // =============================================================================
    using FeatureCreateFn       = uint32_t(*)(void* scene, const void* param, size_t param_size);
    using FeatureRegisterOpsFn  = uint32_t(*)(void* dispatcher, TypeId* out_ops, uint32_t max_ops);
    using FeatureUnregisterOpsFn = void(*)(void* dispatcher, const TypeId* ops, uint32_t op_count);

    struct FeatureFactory
    {
        FeatureCreateFn        create_fn{};
        FeatureRegisterOpsFn   register_ops_fn{};
        FeatureUnregisterOpsFn unregister_ops_fn{};
        /// Stable feature name (== RenderFeature::name()). Lets a client key a
        /// name→{handle, ops} store (no hard-coded indices) and map the scene's
        /// queryFeatures() back to a feature's ops. A plugin supplies its own name
        /// here, so the editor discovers + edits it with zero compile-time coupling.
        const char*            name{""};
        /// Index into this factory's registered ops[] that is the GENERIC setParams
        /// op (a SetFeatureParamsPayload handler — FeatureParamsOperation.hpp), or -1
        /// when the feature exposes no editable params. Lets the settings panel push
        /// a reflected param blob to the right op BY NAME without knowing the
        /// feature's concrete type (so a plugin's params are editable too).
        int                    param_set_op_index{-1};
    };
    static_assert(std::is_trivially_copyable_v<FeatureFactory>);

    inline FeatureFactory makeSimpleFactory(FeatureCreateFn create_fn, const char* name = "") noexcept
    {
        return FeatureFactory{
            create_fn,
            +[](void*, TypeId*, uint32_t) -> uint32_t { return 0u; },
            +[](void*, const TypeId*, uint32_t) {},
            name
        };
    }

    // =============================================================================
    //  Command Payloads — all trivially copyable
    // =============================================================================

    // ---- Scene lifecycle ----

    struct CreateScenePayload
    {
        char name[64]{};
        uint32_t flags{0};

        /// Pipeline format — RGBA16_SFLOAT = HDR, RGBA8_SRGB = LDR.
        lux::common::ETextureFormat lit_color_format{lux::common::ETextureFormat::RGBA16_SFLOAT};

        /// Optional initial views to create atomically with the scene.
        struct ViewInit
        {
            common::Size2D extent{};
            char     name[32]{};
        };
        uint32_t view_count{0};
        ViewInit views[4]{};
    };
    static_assert(std::is_trivially_copyable_v<CreateScenePayload>);

    struct DestroyScenePayload
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<DestroyScenePayload>);

    struct AddViewPayload
    {
        RenderSceneId scene_id{};
        common::Size2D extent{};
        char name[64]{};
        // (initial camera removed — View 去 3D 化: AddView is neutral; the client sends a
        //  StandardViewCamera op for this view after addView. See ViewCameraOperation.hpp.)
    };
    static_assert(std::is_trivially_copyable_v<AddViewPayload>);

    struct RemoveViewPayload
    {
        RenderSceneId scene_id{};
        ViewHandle view{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveViewPayload>);

    struct ResizeViewPayload
    {
        RenderSceneId scene_id{};
        ViewHandle view{};
        common::Size2D new_extent{};
    };
    static_assert(std::is_trivially_copyable_v<ResizeViewPayload>);

    struct BindSwapchainPayload
    {
        RenderSceneId scene_id{};
        ViewHandle view{};
    };
    static_assert(std::is_trivially_copyable_v<BindSwapchainPayload>);

    struct SetSceneTimePayload
    {
        RenderSceneId scene_id{};
        float delta_time{0.f};
        float total_time{0.f};
        uint64_t frame_number{0};
    };
    static_assert(std::is_trivially_copyable_v<SetSceneTimePayload>);

    struct PickPayload
    {
        RenderSceneId scene_id{};
        float origin[3]{};
        float direction[3]{};
    };
    static_assert(std::is_trivially_copyable_v<PickPayload>);

    /// Debug: ask the render thread to write the scene's CURRENT compiled render
    /// graph (human-readable text) into a CALLER-OWNED buffer — the same in-
    /// memory `dst_ptr` idiom as ReadbackView (no file I/O, no fixed-size buffer
    /// in the payload). The render thread shares the address space; the caller
    /// keeps `dst` alive until the request resolves (poll it from the UI loop —
    /// do NOT syncCall from inside a frame). The reply reports `needed` (full
    /// text size) so the caller can resize + re-issue if the buffer was too small.
    struct DumpRenderGraphPayload
    {
        RenderSceneId scene_id{};
        uint64_t      dst_ptr{0};       ///< reinterpret_cast<uintptr_t> of caller buffer
        uint64_t      dst_capacity{0};  ///< bytes available at dst_ptr
    };
    static_assert(std::is_trivially_copyable_v<DumpRenderGraphPayload>);

    /// Enumerate the scene's render features + their reflectable params into a
    /// CALLER-OWNED buffer (same in-memory `dst_ptr` idiom as DumpRenderGraph —
    /// no fixed-size arrays in the payload). The render thread packs one record
    /// per feature; the reply reports `count` + `needed` so the caller can resize
    /// + re-issue if its buffer was too small. Packed record layout (little-end):
    ///   u32 feature_id | u8 enabled
    ///   u16 name_len    + name bytes
    ///   u16 sname_len   + param-struct-name bytes  (0 if the feature has no params)
    ///   u16 param_size  + current param bytes      (0 if the feature has no params)
    struct QueryFeatureParamsPayload
    {
        RenderSceneId scene_id{};
        uint64_t      dst_ptr{0};       ///< reinterpret_cast<uintptr_t> of caller buffer
        uint64_t      dst_capacity{0};  ///< bytes available at dst_ptr
    };
    static_assert(std::is_trivially_copyable_v<QueryFeatureParamsPayload>);

    /// GPU->CPU readback of a view's rendered color image into a CALLER-OWNED
    /// buffer. `dst_ptr` is the address of a client buffer that stays valid for
    /// the (blocking) duration of the request — the server writes the pixels
    /// there directly (the output mirror of pushBorrowedBytes), bypassing the
    /// small reply-payload cap. Pixels are copied in the view's NATIVE color
    /// format (HDR vs LDR is decided by the view/scene, not this command).
    struct ReadbackViewPayload
    {
        RenderSceneId scene_id{};
        ViewHandle    view{};
        uint64_t      dst_ptr{0};       ///< reinterpret_cast<uintptr_t> of caller buffer
        uint64_t      dst_capacity{0};  ///< bytes available at dst_ptr
    };
    static_assert(std::is_trivially_copyable_v<ReadbackViewPayload>);

    /// Async (non-blocking) variant of ReadbackViewPayload. The server submits
    /// the image->buffer copy WITHOUT waiting on the fence, polls it across
    /// ticks, and sends a DEFERRED reply matched by request_id (exactly like
    /// uploadMesh/createTexture2D). `settle_frames` render ticks elapse before
    /// the copy is taken, so static preview content settles into FIF slot 0.
    /// `dst_ptr` is a client buffer that MUST stay valid until the reply
    /// arrives (the server writes the pixels there directly).
    struct ReadbackViewAsyncPayload
    {
        RenderSceneId scene_id{};
        ViewHandle    view{};
        uint64_t      dst_ptr{0};        ///< reinterpret_cast<uintptr_t> of caller buffer
        uint64_t      dst_capacity{0};   ///< bytes available at dst_ptr
        uint32_t      settle_frames{3};  ///< render ticks before the copy is submitted
    };
    static_assert(std::is_trivially_copyable_v<ReadbackViewAsyncPayload>);

    // Mesh-instance command payloads (AddMeshInstance / RemoveMeshInstance /
    // Make|HideInstanceForView / UpdateInstanceFlags|RenderState|UserMeta) moved
    // to a feature header: renderer/features/meshstack/MeshStackOperation.hpp.
    // Mesh instances are a feature domain (StandardMeshStack, dynamic ids via
    // register_ops_fn). The core protocol no longer names them. The UNIVERSAL
    // kInstanceFlag* bits (above) stay — any instanced consumer's cull shader
    // reads them; feature-specific bits go to bits 3+.

    // (The bone-palette / bone-batch payloads moved to a feature header:
    //  renderer/features/skinning/SkinningOperation.hpp. Skinning is a feature
    //  domain, not a core protocol op — see contract C5 in the decoupling design.)

    // ---- Feature type registration ----

    struct RegisterFeatureTypePayload
    {
        FeatureFactory factory{};
    };
    static_assert(std::is_trivially_copyable_v<RegisterFeatureTypePayload>);

    struct UnregisterFeatureTypePayload
    {
        std::uint32_t feature_type_id{0};
    };
    static_assert(std::is_trivially_copyable_v<UnregisterFeatureTypePayload>);

    // ---- Feature instance lifecycle ----

    struct AddFeaturePayload
    {
        RenderSceneId   scene_id{};
        std::uint32_t   feature_type_id{0}; // assigned by RegisterFeatureType reply
        std::uint32_t   attachment_index{0}; // index into AttachmentRecord table (cold path)
    };
    static_assert(std::is_trivially_copyable_v<AddFeaturePayload>);

    struct RemoveFeaturePayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveFeaturePayload>);

    struct SetFeatureEnabledPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        bool enabled{true};
    };
    static_assert(std::is_trivially_copyable_v<SetFeatureEnabledPayload>);

    // ---- Scene activation / bulk-data context ----

    struct SetActiveScenePayload
    {
        RenderSceneId scene_id{};
        bool enabled{true};
    };
    static_assert(std::is_trivially_copyable_v<SetActiveScenePayload>);

    // ---- ImGui draw data submission ----

    struct SubmitImGuiDrawDataPayload
    {
        RenderSceneId scene_id{};      ///< Which ImGui scene to render to
        std::uint32_t attachment_index{0}; ///< Index into AttachmentRecord (ImDrawDataSnapshot)
    };
    static_assert(std::is_trivially_copyable_v<SubmitImGuiDrawDataPayload>);

    // ---- Name-based TypeId query ----

    struct QueryTypeIdPayload
    {
        char name[64]{};               ///< Pre-agreed handler name to look up
    };
    static_assert(std::is_trivially_copyable_v<QueryTypeIdPayload>);

    struct QueryTypeIdReply
    {
        TypeId type_id{kInvalidTypeId}; ///< kInvalidTypeId if not found
        OpCode opcode{0xff};            ///< opcode domain of the found TypeId
    };
    static_assert(std::is_trivially_copyable_v<QueryTypeIdReply>);

    // ---- Swapchain-scene binding ----

    struct RequestSwapchainScenePayload
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<RequestSwapchainScenePayload>);

    struct SwapchainBoundReply
    {
        ViewHandle view{};
        std::uint32_t status{0}; // 0 = ok, 1 = no swapchain, 2 = scene not found
    };
    static_assert(std::is_trivially_copyable_v<SwapchainBoundReply>);

    // ---- BulkData payloads ----

    // TransformWriteEntry (per-frame instance transform batch) moved to
    // renderer/features/meshstack/MeshStackOperation.hpp (StandardMeshStack
    // feature domain). The core protocol no longer names mesh transforms.

    // ViewFrameUpdatePayload (per-view camera matrices) moved to a feature header:
    // renderer/features/view_camera/ViewCameraOperation.hpp (ViewCameraUpdatePayload,
    // StandardViewCamera feature domain). The core protocol no longer names camera data.

    // =============================================================================
    //  Reply Payloads — all trivially copyable
    // =============================================================================

    struct SceneCreatedReply
    {
        RenderSceneId scene_id{};
        uint32_t      view_count{0};
        ViewHandle    views[4]{};
    };
    static_assert(std::is_trivially_copyable_v<SceneCreatedReply>);

    struct ViewCreatedReply
    {
        ViewHandle view{};
    };
    static_assert(std::is_trivially_copyable_v<ViewCreatedReply>);

    struct PickResultReply
    {
        PickResult result{};
    };
    static_assert(std::is_trivially_copyable_v<PickResultReply>);

    struct ReadbackViewReply
    {
        uint32_t status{0};           ///< 0 = success; non-zero = error code
        uint32_t width{0};
        uint32_t height{0};
        uint32_t bytes_per_pixel{0};
        uint64_t bytes_written{0};
        uint32_t format{0};           ///< lux::common::ETextureFormat of written pixels
    };
    static_assert(std::is_trivially_copyable_v<ReadbackViewReply>);

    /// Reply for DumpRenderGraph. `needed` = full dump size in bytes; `written`
    /// = bytes actually copied into the caller buffer (min(needed, capacity)).
    /// If needed > capacity the caller should resize to `needed` and re-issue.
    struct RenderGraphDumpReply
    {
        uint32_t status{0};    ///< 0 = ok; non-zero = error (e.g. scene not found)
        uint32_t needed{0};    ///< full dump size in bytes
        uint32_t written{0};   ///< bytes copied into the caller buffer
    };
    static_assert(std::is_trivially_copyable_v<RenderGraphDumpReply>);

    /// Reply for QueryFeatureParams. `count` = feature records packed into the
    /// caller buffer; `needed` = full packed size; `written` = bytes copied
    /// (min(needed, capacity)). If needed > capacity nothing is written
    /// (count = written = 0) — resize to `needed` and re-issue.
    struct QueryFeatureParamsReply
    {
        uint32_t status{0};    ///< 0 = ok; non-zero = error (e.g. scene not found)
        uint32_t needed{0};    ///< full packed size in bytes
        uint32_t written{0};   ///< bytes copied into the caller buffer
        uint32_t count{0};     ///< number of feature records written
    };
    static_assert(std::is_trivially_copyable_v<QueryFeatureParamsReply>);

    // MeshInstanceSlotReply (addMeshInstance reply) moved to a feature header:
    // renderer/features/meshstack/MeshStackOperation.hpp.

    struct GenericOkReply
    {
        uint32_t code{0};
    };
    static_assert(std::is_trivially_copyable_v<GenericOkReply>);

    // ---- Per-resource-type replies ----

    // MeshUploadedReply stays CORE: it is emitted by the SHARED async-upload worker
    // (mesh + texture transfers — RenderServer drain), not the (now feature-scoped) mesh
    // upload op. CommandTraits<UploadMeshPayload> moved to MeshStackOperation.hpp and
    // reuses the ReplyMeshUploaded id below.
    struct MeshUploadedReply
    {
        RMeshHandle handle{};
        uint32_t status{0}; // 0 = success
    };
    static_assert(std::is_trivially_copyable_v<MeshUploadedReply>);

    // MaterialUploadedReply moved to a feature header:
    // renderer/features/material/MaterialOperation.hpp (StandardMaterial).

    struct Texture2DCreatedReply
    {
        RTextureHandle handle{};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<Texture2DCreatedReply>);

    struct CubeTextureCreatedReply
    {
        RTextureHandle handle{};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<CubeTextureCreatedReply>);

    // LightCreatedReply moved to renderer/features/light/LightOperation.hpp
    // (reply for the feature-scoped createLight). Core no longer names light.

    struct ShaderCompiledReply
    {
        ShaderHandle shader{};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<ShaderCompiledReply>);

    struct FeatureAddedReply
    {
        FeatureHandle feature{};
    };
    static_assert(std::is_trivially_copyable_v<FeatureAddedReply>);

    struct FeatureTypeRegisteredReply
    {
        std::uint32_t feature_type_id{0};
        std::uint32_t op_count{0};
        TypeId ops[16]{};
    };
    static_assert(std::is_trivially_copyable_v<FeatureTypeRegisteredReply>);

    // =============================================================================
    //  CommandTraits specializations — commands that produce replies
    // =============================================================================

    template <>
    struct CommandTraits<CreateScenePayload>
    {
        using Reply = SceneCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplySceneCreated;
    };

    template <>
    struct CommandTraits<AddViewPayload>
    {
        using Reply = ViewCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyViewCreated;
    };

    template <>
    struct CommandTraits<PickPayload>
    {
        using Reply = PickResultReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyPickResult;
    };

    template <>
    struct CommandTraits<ReadbackViewPayload>
    {
        using Reply = ReadbackViewReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyReadbackView;
    };

    template <>
    struct CommandTraits<ReadbackViewAsyncPayload>
    {
        using Reply = ReadbackViewReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyReadbackView;
    };

    template <>
    struct CommandTraits<DumpRenderGraphPayload>
    {
        using Reply = RenderGraphDumpReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyRenderGraphDump;
    };

    template <>
    struct CommandTraits<QueryFeatureParamsPayload>
    {
        using Reply = QueryFeatureParamsReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyQueryFeatureParams;
    };

    // CommandTraits<AddMeshInstancePayload> moved to MeshStackOperation.hpp
    // (the feature owns the add-instance reply contract; reply_type_id is now
    // derived from the Reply type, not the core ReplyMeshSlot tag).

    template <>
    struct CommandTraits<AddFeaturePayload>
    {
        using Reply = FeatureAddedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyFeatureAdded;
    };

    template <>
    struct CommandTraits<SetActiveScenePayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGenericOk;
    };

    template <>
    struct CommandTraits<RegisterFeatureTypePayload>
    {
        using Reply = FeatureTypeRegisteredReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyFeatureTypeRegistered;
    };

    // ---- Resource CommandTraits ----

    // CommandTraits<UploadMeshPayload> moved to MeshStackOperation.hpp (the payload is a
    // feature type now; it reuses the shared-infra reply_type_id ReplyMeshUploaded).

    template <>
    struct CommandTraits<CreateTexture2DPayload>
    {
        using Reply = Texture2DCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyTexture2DCreated;
    };

    template <>
    struct CommandTraits<UpdateTexture2DPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGenericOk;
    };

    template <>
    struct CommandTraits<CreateCubeTexturePayload>
    {
        using Reply = CubeTextureCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyCubeTextureCreated;
    };

    template <>
    struct CommandTraits<UpdateCubeTexturePayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGenericOk;
    };

    // (CommandTraits<UploadMaterialPayload> retired in W5a — builtin closure materials removed.)

    // CommandTraits<UploadGraphMaterialPayload> moved to MaterialOperation.hpp
    // (the feature owns the upload reply contract; reply_type_id is now derived
    // from the Reply type, not the core ReplyMaterialUploaded tag).

    // CommandTraits<CreateLightPayload> moved to LightOperation.hpp (the light
    // create op is feature-scoped; its reply binding travels with the payload).

    template <>
    struct CommandTraits<CompileShaderPayload>
    {
        using Reply = ShaderCompiledReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyShaderCompiled;
    };

    template <>
    struct CommandTraits<QueryTypeIdPayload>
    {
        using Reply = QueryTypeIdReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyQueryTypeId;
    };

    // ---- Swapchain-scene binding CommandTraits ----
    template <>
    struct CommandTraits<RequestSwapchainScenePayload>
    {
        using Reply = SwapchainBoundReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplySwapchainBound;
    };

} // namespace lux::render
