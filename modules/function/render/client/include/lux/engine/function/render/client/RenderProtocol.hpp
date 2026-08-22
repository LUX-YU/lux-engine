#pragma once
// ============================================================================
//  RenderProtocol.hpp — Core protocol: scene/view/feature lifecycle + replies
//
//  Resource-specific payloads live in resources/ops/*.hpp.
//  Feature-specific payloads live in renderer/features/*/XxxOperation.hpp.
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/resources/ops/ResourceOperationCommon.hpp>  // MeshUploadedReply / ReplyMeshUploaded
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/FeatureDescriptor.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/description/Image.hpp>

// Resource operation payloads — needed for CommandTraits specializations below
// Mesh data ops moved to a feature: comm/genops/MeshStackOperation.ops.hpp
// (StandardMeshStack, dynamic ids via register_ops_fn). Core no longer names mesh data
// ops; the upload REPLY (MeshUploadedReply, below) stays here — it is emitted by the
// shared async-upload worker (mesh + texture), not a mesh op.
#include <lux/engine/function/render/client/resources/ops/TextureResourceOperation.hpp>
// Material ops moved to a feature: comm/genops/MaterialOperation.ops.hpp
// (StandardMaterial, dynamic ids via register_ops_fn). Core no longer names material.
// The retired ResourceOp ids 8/16/17 (Destroy / UploadGraph / ModifyGraphMaterial) are
// left unused — not reissued — so any stale serialized material command fails closed.
// Light ops moved to a feature: comm/genops/LightOperation.ops.hpp
// (LightFeature, dynamic ids via register_ops_fn). Core no longer names light.
#include <lux/engine/function/render/client/resources/ops/ShaderResourceOperation.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    // Universal per-instance flags understood by the core renderer. Bits 3+ are
    // kInstanceFlag* 已下沉到 resources/mesh/RenderObjectTypes.hpp(本头包含它)——
    // 那些位描述实例语义而非线协议机制,与 EGeometryKind / PassMask 同处才对。
    // feature 位登记表也随之搬去那里。

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
        // 5 was ResizeView — 视图不再自持尺寸账本,改尺寸走
        // ResizeTarget;id 退役不复用(陈旧序列化命令 fail closed)。
        // 6 was SetSceneTime、7 was the old scene-wide Pick command —— 两者
        // **客户端从来没有对应方法**,物理上发不出;服务端 handler 因此恒不可达。
        // World Instance picking now belongs to the Render Cluster feature
        // domain and uses its scissored ID/depth GPU pass, not this command id.
        // id 退役不复用(陈旧序列化命令 fail closed)。
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
        // 24 was RequestSwapchainScene — 命令面零调用方,已消亡(不复用,
        // 陈旧序列化命令 fail closed);createSurfaceRenderTarget 接棒。
        // 25/26 were UploadBonePalette/UploadBoneBatch — skinning is now a feature
        // domain (SkinningOperation.hpp, dynamic ids via register_ops_fn). The core
        // protocol no longer names skinning. Ids 25/26 left unused (not reissued, so
        // any stale serialized command fails closed rather than mis-dispatching).
        inline constexpr TypeId ReadbackTarget      = 27;  ///< GPU->CPU offscreen-target color readback
        inline constexpr TypeId ReadbackTargetAsync = 28;  ///< deferred GPU->CPU readback (reply by request_id)
        inline constexpr TypeId DumpRenderGraph   = 29;  ///< debug: copy the scene's compiled render graph text into a caller buffer (dst_ptr)
        inline constexpr TypeId QueryFeatureParams = 30; ///< enumerate the scene's features + their reflectable params into a caller buffer (dst_ptr)
        inline constexpr TypeId QueryDeviceCaps    = 37; ///< copy the created device's DeviceCaps into a caller buffer (dst_ptr)
        inline constexpr TypeId RebaseSceneOrigin  = 38;
        inline constexpr TypeId QueryGpuTiming     = 39; ///< copy latest fence-retired per-view GPU timing JSON

        // ---- RenderTarget 一等化命令面(设计 §1)----
        inline constexpr TypeId CreateOffscreenTarget = 31;  ///< 建 Offscreen 目标 → ReplyTargetReady
        inline constexpr TypeId DestroyTarget         = 32;  ///< 两阶段销毁受理 → ReplyTargetReleased(Surface 延迟回执)
        inline constexpr TypeId SetLayer              = 33;  ///< target 合成链 set/replace 一层
        inline constexpr TypeId RemoveLayer           = 34;  ///< target 合成链摘一层
        inline constexpr TypeId ResizeTarget          = 35;  ///< Offscreen 目标改尺寸(直达池)
        inline constexpr TypeId CreateSurfaceTarget   = 36;  ///< 建 Surface 目标(原生窗口句柄)→ ReplyTargetReady

        // ---- BulkData domain ----
        // 1 was TransformWrite — moved to the StandardMeshStack feature
        // (MeshStackOperation.hpp TransformBatch op). Left unused.
        // 2 was ViewFrameUpdate — per-view camera is the StandardViewCamera feature op
        // now (ViewCameraOperation.hpp, dynamic id). Left unused (not reissued, so any
        // stale serialized command fails closed).
        // 3 was SceneTimeUpdate —— 全仓零引用:无 payload、无 handler、无发送方,
        // 是个孤立常量。随 SetSceneTime 一并退役。
        // 4 (LightUpdate) moved to a feature: LightOperation.hpp light batch op
        // (dynamic id via register_ops_fn). Core no longer names light.

        // ---- Reply domain ----
        inline constexpr TypeId ReplySceneCreated = 1;
        inline constexpr TypeId ReplyViewCreated = 2;
        // 3 was ReplyPickResult —— 随 Pick 一并退役。
        // 4 was ReplyMeshSlot — the addMeshInstance reply is feature-scoped now
        // (MeshStackOperation.hpp; reply_type_id derived from the Reply type).
        inline constexpr TypeId ReplyGenericOk = 5;
        inline constexpr TypeId ReplyFeatureAdded = 6;
        inline constexpr TypeId ReplyFeatureTypeRegistered = 8;
        // ReplyMeshUploaded(=10)已下沉到 resources/ops/ResourceOperationCommon.hpp
        // 11 was ReplyMaterialUploaded — the upload reply is feature-scoped now
        // (MaterialOperation.hpp; reply_type_id derived from the Reply type).
        inline constexpr TypeId ReplyTexture2DCreated = 12;
        inline constexpr TypeId ReplyCubeTextureCreated = 13;
        // 14 (ReplyLightCreated) moved to LightOperation.hpp (reply tag for the
        // feature-scoped createLight; value reserved). Core no longer names light.
        inline constexpr TypeId ReplyShaderCompiled = 15;
        inline constexpr TypeId ReplyReadbackTarget = 20;
        inline constexpr TypeId ReplyTextureRegionsApplied = 23;   ///< U2-00 region-batch ack
        inline constexpr TypeId ReplyTextureMipRangeReplaced = 29;
        inline constexpr TypeId ReplyTextureMipDemands = 30;

        // ---- Name-based TypeId query ----
        inline constexpr TypeId QueryTypeId      = 22;
        inline constexpr TypeId ReplyQueryTypeId = 17;

        // ---- Debug ----
        inline constexpr TypeId ReplyRenderGraphDump = 21;
        inline constexpr TypeId ReplyQueryFeatureParams = 22;  ///< NOTE: reply-id space is separate from the command-id space (QueryTypeId command=22)

        // ---- Device capability query ----
        inline constexpr TypeId ReplyDeviceCaps = 26;
        inline constexpr TypeId ReplyGpuTiming = 31;

        // ---- 渲染线程自发上报(无对应请求,request_id = kInvalidRequestId)----
        inline constexpr TypeId ReplyErrorEventBatch = 27;  ///< 一批的封面{count, dropped}
        inline constexpr TypeId ReplyErrorEvent      = 28;  ///< 批内的一条 RenderErrorEvent

        // ---- Swapchain-scene binding ----
        // 19 was ReplySwapchainBound — 随 RequestSwapchainScene 消亡(不复用)。
        inline constexpr TypeId ReplyTargetReady    = 24;  ///< RenderTarget 创建回执{target_id}
        inline constexpr TypeId ReplyTargetReleased = 25;  ///< DestroyTarget 回执(Surface 两阶段延迟)

        // ---- Generic dispatch failure ----
        // Emitted by the dispatcher for ANY reply-expecting command that fails to
        // dispatch (bad opcode / payload OOB / unknown or stale TypeId / handler
        // rejection). Routed to the originating request by request_id so the client
        // unblocks with a failure instead of hanging. Reserved high value (see
        // RenderCommTypes.hpp) — not part of the hand-assigned reply-id space.
        inline constexpr TypeId ReplyCommandFailed = kReplyCommandFailedTypeId;
    } // namespace type_ids

    // FeatureFactory / FeatureCreateFn / makeSimpleFactory 已下沉到
    // core/protocol/FeatureFactory.hpp(本头顶部包含它,对外接口不变)。
    // 它只依赖 FeatureHandle / TypeId / FeatureDescriptor,本就是基础层契约;
    // 留在这里会逼 47 个渲染层文件为一个结构包含整个线协议头。

    // =============================================================================
    //  Command Payloads — all trivially copyable
    // =============================================================================

    // ---- Scene lifecycle ----

    struct CreateScenePayload
    {
        char name[64]{};
        uint32_t flags{0};

        /// Pipeline format — RGBA16_SFLOAT = HDR, RGBA8_SRGB = LDR.
        lux::rdesc::ETextureFormat lit_color_format{lux::rdesc::ETextureFormat::RGBA16_SFLOAT};

        double coordinate_page_size{1024.0};
        std::int64_t scene_origin_page[3]{};

        // (原"初始视图"数组已删:视图不再隐带渲染目标,创建后经
        //  CreateOffscreenTarget + SetLayer 显式接线,原子建组无意义。)
    };
    static_assert(std::is_trivially_copyable_v<CreateScenePayload>);

    struct DestroyScenePayload
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<DestroyScenePayload>);

    struct RebaseSceneOriginPayload
    {
        RenderSceneId scene_id{};
        std::int64_t scene_origin_page[3]{};
    };
    static_assert(std::is_trivially_copyable_v<RebaseSceneOriginPayload>);

    struct AddViewPayload
    {
        RenderSceneId scene_id{};
        lux::math::Extent2u extent{};
        char name[64]{};
        // (initial camera removed — now that View is no longer inherently 3D, AddView is neutral; the client sends a
        //  StandardViewCamera op for this view after addView. See ViewCameraOperation.hpp.)
    };
    static_assert(std::is_trivially_copyable_v<AddViewPayload>);

    struct RemoveViewPayload
    {
        RenderSceneId scene_id{};
        ViewHandle view{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveViewPayload>);

    // (ResizeViewPayload 已消亡:视图不再自持尺寸账本——改尺寸走
    //  ResizeTargetPayload 直达图像池,渲染期 extent 从 binding 派生。M2c。)

    struct BindSwapchainPayload
    {
        RenderSceneId scene_id{};
        ViewHandle view{};
    };
    static_assert(std::is_trivially_copyable_v<BindSwapchainPayload>);

    // ── RenderTarget 一等化命令载荷(设计 §1;View 是纯视角,Target 是
    //    输出容器,层链决定合成)────────────────────────────────────────

    /// 建 Offscreen 目标。flags bit0(kTargetFlagSampled):色图带 SAMPLED
    /// usage、final_layout=SHADER_READ_ONLY——可被 ImGui/后续 pass 采样
    /// (原 addUIView 的 layout 形态);不带则可 TRANSFER_SRC readback
    /// (原 addView 形态)。格式当前统一 B8G8R8A8_SRGB + D32(format_key
    /// 定制随编译 key 拆解开放)。
    struct CreateOffscreenTargetPayload
    {
        lux::math::Extent2u extent{};
        uint32_t       flags{0};
    };
    static_assert(std::is_trivially_copyable_v<CreateOffscreenTargetPayload>);
    inline constexpr uint32_t kTargetFlagSampled = 1u << 0;

    /// 建 Surface 目标:POD 原生窗口句柄随命令跨线程,surface/
    /// swapchain 的创建只发生在渲染线程(所有权铁律)。extent = 宿主读到
    /// 的 framebuffer 初始尺寸;此后由 swapchain rebuild 按 caps 跟随。
    struct CreateSurfaceTargetPayload
    {
        uint64_t       native_window_handle{0};  ///< Win32 = HWND;Android 口子留待实机
        lux::math::Extent2u extent{};
    };
    static_assert(std::is_trivially_copyable_v<CreateSurfaceTargetPayload>);

    struct DestroyTargetPayload
    {
        RenderTargetId target{};
    };
    static_assert(std::is_trivially_copyable_v<DestroyTargetPayload>);

    /// DestroyTarget 的回执(两阶段销毁的后半程):Offscreen 受理即回
    /// (池按 fence 水位延迟拆,客户端无需等待);Surface 在 swapchain/
    /// surface 真正拆完后延迟回执——宿主收到它才允许销毁平台窗口
    /// (surface 派生资源 ≤ surface ≤ 平台窗口,生命周期不变量①)。
    struct TargetReleasedReply
    {
        RenderTargetId target{};
        uint32_t       status{0};   ///< 0 = released;1 = 目标不存在(幂等)
    };
    static_assert(std::is_trivially_copyable_v<TargetReleasedReply>);

    /// 设置合成链第 order 层(存在即替换)。层引用 (scene, view),不拥有
    /// ——view 销毁时层被自动摘除(生命周期不变量③)。
    struct SetLayerPayload
    {
        RenderTargetId target{};
        uint32_t       order{0};
        RenderSceneId  scene_id{};
        ViewHandle     view{};
    };
    static_assert(std::is_trivially_copyable_v<SetLayerPayload>);

    struct RemoveLayerPayload
    {
        RenderTargetId target{};
        uint32_t       order{0};
    };
    static_assert(std::is_trivially_copyable_v<RemoveLayerPayload>);

    struct ResizeTargetPayload
    {
        RenderTargetId target{};
        lux::math::Extent2u new_extent{};
    };
    static_assert(std::is_trivially_copyable_v<ResizeTargetPayload>);

    struct TargetReadyReply
    {
        RenderTargetId target{};
        uint32_t       status{0};   ///< 0 = ok
    };
    static_assert(std::is_trivially_copyable_v<TargetReadyReply>);

    //(SetSceneTimePayload / PickPayload 已随 id 6/7 退役 —— 见上方 type_ids
    // 的退役注释。)

    /// Debug: ask the render thread to write the scene's CURRENT compiled render
    /// graph (human-readable text) into a CALLER-OWNED buffer — the same in-
    /// memory `dst_ptr` idiom as ReadbackTarget (no file I/O, no fixed-size buffer
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

    struct QueryGpuTimingPayload
    {
        RenderSceneId scene_id{};
        uint64_t      dst_ptr{0};
        uint64_t      dst_capacity{0};
    };
    static_assert(std::is_trivially_copyable_v<QueryGpuTimingPayload>);

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

    /// 查询设备实际启用了什么能力(`DeviceCaps` 整块)。
    ///
    /// 这是"决策权归上层"的物理前提:引擎不再替客户端在延迟/前向、local-read/采样
    /// 之间选路,那客户端就必须先看得见设备能做什么。走 `dst_ptr` 而不是定长回复,
    /// 因为 `DeviceCaps` 会随适配面持续长大,塞进回复迟早撞上载荷上限。
    struct QueryDeviceCapsPayload
    {
        uint64_t dst_ptr{0};       ///< reinterpret_cast<uintptr_t> of caller's DeviceCaps
        uint64_t dst_capacity{0};  ///< bytes available at dst_ptr
    };
    static_assert(std::is_trivially_copyable_v<QueryDeviceCapsPayload>);

    /// GPU->CPU readback of an offscreen render target's image into a
    /// CALLER-OWNED buffer. `dst_ptr` is the address of a client buffer that
    /// stays valid for the (blocking) duration of the request — the server
    /// writes the pixels there directly, bypassing the small reply-payload cap. Pixels are
    /// copied in the target's NATIVE color format.
    struct ReadbackTargetPayload
    {
        RenderTargetId target{};
        uint64_t       dst_ptr{0};       ///< reinterpret_cast<uintptr_t> of caller buffer
        uint64_t       dst_capacity{0};  ///< bytes available at dst_ptr
        uint8_t        slot{0};          ///< TargetSlot to read back (0 = SceneColor)
    };
    static_assert(std::is_trivially_copyable_v<ReadbackTargetPayload>);

    /// Async (non-blocking) variant of ReadbackTargetPayload. The server
    /// submits the image->buffer copy WITHOUT waiting on the fence, polls it
    /// across ticks, and sends a DEFERRED reply matched by request_id (exactly
    /// like uploadMesh/createTexture2D). `settle_frames` render ticks elapse
    /// before the copy is taken, so static preview content settles into FIF
    /// slot 0. `dst_ptr` is a client buffer that MUST stay valid until the
    /// reply arrives (the server writes the pixels there directly).
    struct ReadbackTargetAsyncPayload
    {
        RenderTargetId target{};
        uint64_t       dst_ptr{0};        ///< reinterpret_cast<uintptr_t> of caller buffer
        uint64_t       dst_capacity{0};   ///< bytes available at dst_ptr
        uint32_t       settle_frames{3};  ///< render ticks before the copy is submitted
        uint8_t        slot{0};           ///< TargetSlot to read back (0 = SceneColor)
    };
    static_assert(std::is_trivially_copyable_v<ReadbackTargetAsyncPayload>);

    // Mesh-instance command payloads (AddMeshInstance / RemoveMeshInstance /
    // Make|HideInstanceForView / UpdateInstanceFlags|RenderState|UserMeta) moved
    // to a feature header: comm/genops/MeshStackOperation.ops.hpp.
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
        std::uint32_t module_lease_attachment{kInvalidTypeId};
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

    // (RequestSwapchainScenePayload / SwapchainBoundReply 已消亡:命令面零
    //  调用方;上屏走 BindSwapchain,由 createSurfaceRenderTarget 接棒。)

    // ---- BulkData payloads ----

    // TransformWriteEntry (per-frame instance transform batch) moved to
    // comm/genops/MeshStackOperation.ops.hpp (StandardMeshStack
    // feature domain). The core protocol no longer names mesh transforms.

    // ViewFrameUpdatePayload (per-view camera matrices) moved to a feature header:
    // comm/genops/ViewCameraOperation.ops.hpp (ViewCameraUpdatePayload,
    // StandardViewCamera feature domain). The core protocol no longer names camera data.

    // =============================================================================
    //  Reply Payloads — all trivially copyable
    // =============================================================================

    /// CreateScene 回执。`scene_id` 无效时 `error` 说明原因;成功时 `error.ok()`。
    struct SceneCreatedReply
    {
        RenderSceneId scene_id{};
        RenderError   error{};
    };
    static_assert(std::is_trivially_copyable_v<SceneCreatedReply>);

    /// AddView 回执。`view` 无效时 `error` 说明原因;成功时 `error.ok()`。
    struct ViewCreatedReply
    {
        ViewHandle  view{};
        RenderError error{};
    };
    static_assert(std::is_trivially_copyable_v<ViewCreatedReply>);

    //(PickResultReply 已随 Pick 退役;core/PickResult.hpp 亦随之删除 ——
    // 编辑器拾取走 SceneController → EditorScene::onPick,与渲染协议无关。)

    struct ReadbackTargetReply
    {
        uint32_t status{0};           ///< 0 = success; non-zero = error code
        uint32_t width{0};
        uint32_t height{0};
        uint32_t bytes_per_pixel{0};
        uint64_t bytes_written{0};
        uint32_t format{0};           ///< lux::rdesc::ETextureFormat of written pixels
    };
    static_assert(std::is_trivially_copyable_v<ReadbackTargetReply>);

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

    struct GpuTimingReply
    {
        uint32_t status{0};
        uint32_t needed{0};
        uint32_t written{0};
    };
    static_assert(std::is_trivially_copyable_v<GpuTimingReply>);

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

    /// Reply for QueryDeviceCaps. `version` 是服务端编译期的 `kDeviceCapsVersion` ——
    /// 与客户端自己的常量不符时,`dst` 里的字节按错位处理,别读(见 DeviceCaps.hpp
    /// 的布局纪律)。`needed` 恒为 `sizeof(DeviceCaps)`,缓冲不够时不写入。
    struct DeviceCapsReply
    {
        RenderError error{};
        uint32_t    version{0};
        uint32_t    needed{0};
        uint32_t    written{0};
        uint32_t    feature_level{0};   ///< EFeatureLevel:引擎按 caps 归纳出的分级
    };
    static_assert(std::is_trivially_copyable_v<DeviceCapsReply>);

    // MeshInstanceSlotReply (addMeshInstance reply) moved to a feature header:
    // comm/genops/MeshStackOperation.ops.hpp.

    // GenericOkReply 已下沉到 core/protocol/RenderCommTypes.hpp(本头包含它)。
    static_assert(std::is_trivially_copyable_v<GenericOkReply>);

    // EDispatchFailure / CommandFailedReply 已下沉到 core/protocol/
    // RenderCommTypes.hpp(本头包含它)—— 客户端 RenderRequest 要解码失败回复,
    // 与 GenericOkReply 同一归层理由。

    // ---- Per-resource-type replies ----

    // MeshUploadedReply stays CORE: it is emitted by the SHARED async-upload worker
    // (mesh + texture transfers — RenderServer drain), not the (now feature-scoped) mesh
    // upload op. CommandTraits<UploadMeshPayload> moved to MeshStackOperation.hpp and
    // reuses the ReplyMeshUploaded id below.
    // MeshUploadedReply 已下沉到 resources/ops/ResourceOperationCommon.hpp
    // (连同 type_ids::ReplyMeshUploaded)—— 它归共享上传基础设施所有,留在这里
    // 会逼 MeshStackOperation.hpp 为一个常量包含整个线协议头。
    static_assert(std::is_trivially_copyable_v<MeshUploadedReply>);

    // MaterialUploadedReply moved to a feature header:
    // comm/genops/MaterialOperation.ops.hpp (StandardMaterial).

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

    /// Reply for UpdateTextureRegions (U2-00): echoes the batch's content_revision so
    /// the producer advances its uploaded_revision ONLY on status == Ok
    /// (ERegionUploadStatus) — a refused batch leaves the dirty state pending for
    /// retry instead of being silently lost (U2-03 acceptance).
    struct TextureRegionsAppliedReply
    {
        uint64_t content_revision{0};
        uint32_t status{0};   ///< numeric ERegionUploadStatus
        uint32_t reserved_{0};
    };
    static_assert(std::is_trivially_copyable_v<TextureRegionsAppliedReply>);

    /// Terminal reply for ReplaceTexture2DMipRange. A successful reply echoes
    /// the SAME handle and logical base mip; a failure never destroys or mutates
    /// the previously resident image.
    struct TextureMipRangeReplacedReply
    {
        RTextureHandle handle{};
        std::uint32_t base_mip{0u};
        std::uint32_t status{0u};
    };
    static_assert(std::is_trivially_copyable_v<TextureMipRangeReplacedReply>);

    // LightCreatedReply moved to comm/genops/LightOperation.ops.hpp
    // (reply for the feature-scoped createLight). Core no longer names light.

    struct ShaderCompiledReply
    {
        ShaderHandle shader{};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<ShaderCompiledReply>);

    /// AddFeature 回执。`feature` 无效时 `error` 说明原因 —— 场景不存在、特性类型
    /// 未注册、载荷版本不符、工厂内部装配失败,客户端据此分诊而不是只看到一个
    /// 无效句柄。成功时 `error.ok()` 为真。
    struct FeatureAddedReply
    {
        FeatureHandle feature{};
        RenderError   error{};
    };
    static_assert(std::is_trivially_copyable_v<FeatureAddedReply>);

    struct FeatureTypeRegisteredReply
    {
        std::uint32_t feature_type_id{0};   ///< >=1 on success, 0 on failure (see `error`)
        std::uint32_t op_count{0};
        TypeId ops[16]{};
        /// EFeatureTypeRegisterStatus on success (0 = Registered, 1 = AlreadyRegistered).
        /// Only meaningful when feature_type_id != 0. Kept a raw uint32 so this header
        /// stays free of the renderer-side enum (FeatureTypeRegistry.hpp) it rides.
        std::uint32_t status{0};
        /// feature_type_id == 0 时说明为什么(服务器未初始化、工厂没有 create_fn、
        /// 稳定类型 id 撞车……)。此前这个原因只打在服务端 stderr 上,客户端拿到的
        /// 只有一个 0。
        RenderError   error{};
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

    // View 与 feature 是同一族(有界槽位 + 代次 + 级联摘层 + 特性每视图状态的
    // 对称释放),所以 removeView 向 removeFeature/destroyTarget 看齐带回执,
    // 而不是向 destroyTexture(全局资源、无界池)看齐。会被拒的方式:场景不存在
    // (句柄陈旧/已销毁)、视图不可摘(重复摘/已在销毁中)。此前即发即忘,被拒时
    // 客户端毫不知情 —— 有界的视图池摘没摘掉,它只能猜。
    template <>
    struct CommandTraits<RemoveViewPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGenericOk;
    };

    template <>
    struct CommandTraits<CreateOffscreenTargetPayload>
    {
        using Reply = TargetReadyReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyTargetReady;
    };

    template <>
    struct CommandTraits<CreateSurfaceTargetPayload>
    {
        using Reply = TargetReadyReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyTargetReady;
    };

    template <>
    struct CommandTraits<DestroyTargetPayload>
    {
        using Reply = TargetReleasedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyTargetReleased;
    };

    template <>
    struct CommandTraits<ReadbackTargetPayload>
    {
        using Reply = ReadbackTargetReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyReadbackTarget;
    };

    template <>
    struct CommandTraits<ReadbackTargetAsyncPayload>
    {
        using Reply = ReadbackTargetReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyReadbackTarget;
    };

    template <>
    struct CommandTraits<DumpRenderGraphPayload>
    {
        using Reply = RenderGraphDumpReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyRenderGraphDump;
    };

    template <>
    struct CommandTraits<QueryGpuTimingPayload>
    {
        using Reply = GpuTimingReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGpuTiming;
    };

    template <>
    struct CommandTraits<QueryFeatureParamsPayload>
    {
        using Reply = QueryFeatureParamsReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyQueryFeatureParams;
    };

    template <>
    struct CommandTraits<QueryDeviceCapsPayload>
    {
        using Reply = DeviceCapsReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyDeviceCaps;
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

    // 卸载 / 启停都会被拒:反向依赖挡着、句柄过期、特性声明了不可运行期关闭。
    // 此前两者都是 fire-and-forget,被拒时客户端毫不知情 —— 它以为特性已经没了
    // 或者已经关了,后续的判断全建立在这个错误认知上。
    template <>
    struct CommandTraits<RemoveFeaturePayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGenericOk;
    };

    template <>
    struct CommandTraits<SetFeatureEnabledPayload>
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

    template <>
    struct CommandTraits<UnregisterFeatureTypePayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyGenericOk;
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
    struct CommandTraits<ReplaceTexture2DMipRangePayload>
    {
        using Reply = TextureMipRangeReplacedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id =
            type_ids::ReplyTextureMipRangeReplaced;
    };

    template <>
    struct CommandTraits<QueryTextureMipDemandsPayload>
    {
        using Reply = TextureMipDemandsReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id =
            type_ids::ReplyTextureMipDemands;
    };

    template <>
    struct CommandTraits<CreateCubeTexturePayload>
    {
        using Reply = CubeTextureCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyCubeTextureCreated;
    };

    template <>
    struct CommandTraits<CreatePersistentTexture2DPayload>
    {
        // Same reply shape as any texture create: {handle, status}. status carries
        // ERegionUploadStatus (including retryable CapacityExhausted).
        using Reply = Texture2DCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyTexture2DCreated;
    };

    template <>
    struct CommandTraits<UpdateTextureRegionsPayload>
    {
        using Reply = TextureRegionsAppliedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = type_ids::ReplyTextureRegionsApplied;
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

} // namespace lux::render
