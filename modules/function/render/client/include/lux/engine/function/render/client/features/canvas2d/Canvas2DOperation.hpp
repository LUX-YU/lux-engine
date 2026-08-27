#pragma once
// ============================================================================
//  Canvas2DOperation.hpp — the public protocol between the 2D renderable bridges
//  (gameplay d2) and the render-side Canvas2DFeature (SinglePerScene).
//
//  v2 (C2D-R0, GPU-DRIVEN — .internal/2d-gpu-driven-rewrite.md): images are
//  GPU-RESIDENT instances in the scene's Canvas2D instance arena, maintained by
//  create / destroy / delta commands (the MeshStack paradigm).
//
//  This is the AUTHOR header: the instance records (std430 mirrors), the owner
//  handles, the priority quantization, the create status, and — annotated with
//  LUX_OP / LUX_OP_BLOB — the wire payloads themselves. The op ids, the
//  Canvas2DProxy and the feature descriptor are GENERATED from these annotations
//  into <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>; include THAT
//  header to send commands. The convenience overloads declared at the bottom of
//  this file are hand-written free functions layered over the generated proxy.
//
//  DRAW-ORDER CONTRACT (frozen, decision ① 2026-07-06):
//   - every live+visible instance has key = (quantizePriority(priority) << 32)
//     | slot_index; the canvas draws keys in ASCENDING order;
//   - HIGHER priority ⇒ HIGHER key ⇒ drawn LATER ⇒ ON TOP;
//   - equal priorities tie-break by slot index (≈ creation order) — the key
//     space is a STRICT TOTAL ORDER by construction, deterministically;
//   - the key space is shared by every future canvas producer (tile chunks /
//     pixel-field chunks interleave with images on the same axis).
//   - blend stays premultiplied alpha (Canvas2DAlphaMode).
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/description/Tilemap2D.hpp> // rdesc::kEmptyTile(组件层共用的编码约定)
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/cxx/container/SlotMap.hpp> // lux::cxx::SlotKey (generational owner handles)
#include <lux/engine/function/visibility.h>

#include <bit>
#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    /// Blend contract: premultiplied alpha (kept as an enum so a future
    /// straight-alpha path is a value, not a rewrite).
    enum class Canvas2DAlphaMode : std::uint8_t
    {
        Premultiplied = 0,
        Straight = 1,
    };

    /// Sentinel bindless index meaning "no texture — draw the flat premultiplied tint".
    /// (The bindless allocator can hand out index 0 for a real texture, so 0 can't be the
    /// sentinel.) The image fragment shader branches on it, so an untextured image needs
    /// no default/white texture. Producers set a real index (RTextureHandle::index) once a
    /// texture resolves; the default leaves the draw tint-only.
    inline constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;

    // ════════════════════════════════════════════════════════════════════════
    //  v2 GPU-driven instance protocol (C2D-R0)
    // ════════════════════════════════════════════════════════════════════════

    struct Image2DInstanceTag
    {
    };
    /// Generational per-image owner handle into the scene's Canvas2D instance
    /// arena. A stale handle (generation mismatch) is rejected by every op.
    using Image2DHandle = lux::cxx::SlotKey<Image2DInstanceTag>;

    /// Order-preserving float→u32 map:  a < b  ⇔  quantizePriority(a) <
    /// quantizePriority(b)  for every ordered pair of floats (negatives flip;
    /// -0.0f lands one step below +0.0f — distinct but adjacent, deterministic).
    /// NaN has no float ordering; it maps above +inf — a NaN priority is a
    /// producer contract violation, but the sort stays deterministic (never UB).
    [[nodiscard]] constexpr std::uint32_t quantizePriority(float priority) noexcept
    {
        const auto bits = std::bit_cast<std::uint32_t>(priority);
        return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
    }

    // The order-preservation property is pinned at COMPILE TIME — permanent,
    // zero-cost, cannot rot (C2D-R0 gate).
    static_assert(quantizePriority(-1e30f) < quantizePriority(-1.0f));
    static_assert(quantizePriority(-1.0f) < quantizePriority(-0.5f));
    static_assert(quantizePriority(-0.5f) < quantizePriority(-0.0f));
    static_assert(quantizePriority(-0.0f) < quantizePriority(0.0f));
    static_assert(quantizePriority(0.0f) < quantizePriority(0.5f));
    static_assert(quantizePriority(0.5f) < quantizePriority(1.0f));
    static_assert(quantizePriority(1.0f) < quantizePriority(1e30f));

    /// GPU-resident per-image record — the std430 mirror the image vertex
    /// shader pulls (keep canvas2d/image.vert in sync). Scalar fields only, so
    /// C++ packing == std430 packing (48 B, array stride 48 in an SSBO).
    struct Image2DInstanceData
    {
        /// 2D affine world transform, column-major: [c0.x c0.y c1.x c1.y tx ty].
        /// The producer bakes size×pivot in; the VS expands a unit ±0.5 quad.
        float m[6]{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
        std::int32_t page_delta[2]{};
        float uv[4]{0.f, 0.f, 1.f, 1.f};            ///< atlas rect: u0, v0, w, h
        std::uint32_t tint{0xFFFFFFFFu};            ///< premultiplied RGBA8
        std::uint32_t texture_bindless{kNoTexture}; ///< set-2 index / kNoTexture = tint-only
    };
    static_assert(
        sizeof(Image2DInstanceData) == 56,
        "Image2DInstanceData layout drift — keep canvas2d/image.vert in sync.");
    static_assert(std::is_trivially_copyable_v<Image2DInstanceData>);

    // ── PixelField kind (F2-09) ─────────────────────────────────────────────

    struct PixelFieldInstanceTag
    {
    };
    /// Generational per-field-chunk owner handle into the canvas arena's
    /// PixelField kind store. A stale handle is rejected by every op.
    using PixelFieldInstanceHandle = lux::cxx::SlotKey<PixelFieldInstanceTag>;

    /// GPU-resident per-field-chunk record — the std430 mirror the pixel-field
    /// vertex/fragment shaders pull (keep canvas2d/pixel_field.vert in sync).
    /// The chunk quad is a unit ±0.5 quad placed by `m`; the fragment shader
    /// texelFetches the R16_UNORM material-id mirror (`field_texture`, cell ids
    /// round-trip exactly through the float sampler) and looks the id up in the
    /// 256×1 RGBA8 `palette_texture` (premultiplied; entry 0 = transparent).
    /// Material colour changes therefore update ONLY the palette texture.
    struct PixelField2DInstanceData
    {
        float m[6]{1.f, 0.f, 0.f, 1.f, 0.f, 0.f}; ///< column-major 2D affine (this CHUNK's extent baked in)
        std::int32_t page_delta[2]{};
        std::uint32_t field_texture{
            kNoTexture}; ///< bindless set-2 index of the R16_UNORM id mirror (C2-01: the scene ATLAS)
        std::uint32_t palette_texture{kNoTexture}; ///< bindless set-2 index of the 256×1 RGBA8 palette
        std::uint32_t cells_w{0};                  ///< this chunk's texel extent (texelFetch bounds)
        std::uint32_t cells_h{0};
        std::uint32_t tint{0xFFFFFFFFu}; ///< premultiplied RGBA8 modulate
        /// C2-01: the chunk's texel origin inside the atlas texture (0,0 for a
        /// dedicated texture). texelFetch reads atlas_origin + cell.
        std::uint32_t atlas_x{0};
        std::uint32_t atlas_y{0};
        std::uint32_t _pad0{0};
    };
    static_assert(
        sizeof(PixelField2DInstanceData) == 64,
        "PixelField2DInstanceData layout drift — keep canvas2d/pixel_field.vert in sync.");
    static_assert(std::is_trivially_copyable_v<PixelField2DInstanceData>);

    // ── Offscreen groups (A2-04) ────────────────────────────────────────────

    /// Group 0 = the direct SceneColor path (always present, zero cost).
    /// Groups 1..kMaxCanvas2DGroups render into their own full-view offscreen
    /// RT and are composited (premultiplied) onto SceneColor afterwards — the
    /// mask/post-FX isolation seam. Declared ONCE at feature creation
    /// (Canvas2DCommConfig): the pass topology stays fixed per attach.
    inline constexpr std::uint32_t kMaxCanvas2DGroups = 3;

    /// Wire config for AddFeature("Canvas2D"). Trivially copyable by contract.
    struct LUX_COMM_CONFIG(
        prefix = Canvas2D,
        id = lux.render.canvas2d.v2,
        display = Canvas2D,
        feature = Canvas2DFeature,
        multiplicity = single,
        feature_header = lux / engine / render / renderer / features / canvas2d /
                         Canvas2DFeature.hpp) Canvas2DCommConfig
    {
        std::uint32_t offscreen_groups{0}; ///< clamped to kMaxCanvas2DGroups
    };
    static_assert(std::is_trivially_copyable_v<Canvas2DCommConfig>);

    // ── Tile kind (A2-02) ───────────────────────────────────────────────────

    struct Tile2DInstanceTag
    {
    };
    /// Generational per-tilemap owner handle into the canvas arena's Tile kind
    /// store. A stale handle is rejected by every op.
    using Tile2DInstanceHandle = lux::cxx::SlotKey<Tile2DInstanceTag>;

    /// Tile-id texel meaning "no tile here" in the R16_UNORM index texture.
    ///
    /// 定义搬去了 `lux::rdesc`(description/Tilemap2D.hpp)—— 它是组件层与渲染层
    /// **共同**的编码约定,住在任何一边都会让另一边反向依赖。这里直接用那个名字,
    /// 不留 `inline constexpr auto kEmptyTile = rdesc::kEmptyTile;` 之类的转发别名。
    using lux::rdesc::kEmptyTile;

    /// GPU-resident per-TILEMAP record — the std430 mirror the tile shaders
    /// pull (keep canvas2d/tile.vert in sync). One instance = one whole
    /// tilemap quad (the PixelField shape): the fragment shader texelFetches
    /// the R16_UNORM tile-INDEX texture, derives the tile's uv rect from the
    /// tileset's uniform grid (cols × rows, no margin/spacing in the MVP) and
    /// samples the tileset texture. Editing tiles = a region update of the
    /// index texture — the instance record never changes.
    struct Tile2DInstanceData
    {
        float m[6]{1.f, 0.f, 0.f, 1.f, 0.f, 0.f}; ///< column-major 2D affine (full map extent baked in)
        std::int32_t page_delta[2]{};
        std::uint32_t tileset_texture{kNoTexture}; ///< bindless set-2 index of the tileset atlas
        std::uint32_t index_texture{kNoTexture};   ///< bindless set-2 index of the R16_UNORM tile-id map
        std::uint32_t tiles_w{0};                  ///< index-map texel extent (texelFetch bounds)
        std::uint32_t tiles_h{0};
        std::uint32_t tileset_grid{0};   ///< packed: cols (low 16) | rows (high 16)
        std::uint32_t tint{0xFFFFFFFFu}; ///< premultiplied RGBA8 modulate
        std::uint32_t atlas_x{0};        ///< tile-index atlas texel origin
        std::uint32_t atlas_y{0};
    };
    static_assert(
        sizeof(Tile2DInstanceData) == 64,
        "Tile2DInstanceData layout drift — keep canvas2d/tile.vert in sync.");
    static_assert(std::is_trivially_copyable_v<Tile2DInstanceData>);

    [[nodiscard]] constexpr std::uint32_t packTilesetGrid(std::uint32_t cols, std::uint32_t rows) noexcept
    {
        return (cols & 0xFFFFu) | (rows << 16);
    }

    /// addImage outcome (G-05 discipline, mirrors MeshInstanceCreateStatus):
    /// anything but Ok ⇒ NO instance exists (handle is null) — the client must
    /// not treat it as live. `Unknown` is the DEFAULT on purpose: a generic
    /// dispatch failure delivers a default-constructed reply the server never
    /// filled, so the default must be neither Ok (silent zombie) nor a retriable
    /// error (endless retry). Only CapacityExhausted is transient.
    enum class ECanvas2DCreateStatus : std::uint32_t
    {
        Unknown = 0,
        Ok = 1,
        InvalidConfiguration = 2, ///< scene / Canvas2DFeature absent — permanent
        CapacityExhausted = 3,    ///< arena at max capacity — transient
    };

    /// Create one GPU-resident image instance. Replies with its owner handle +
    /// outcome (G-05: anything but Ok ⇒ handle null, nothing was created).
    struct LUX_OP(lane = frame, kind = stream, name = AddImage2D, method = addImageRaw, reply = Image2DSlotReply)
        AddImage2DPayload
    {
        RenderSceneId scene{};
        Image2DInstanceData data{};
        float priority{0.f};
        std::uint32_t visible{1};
        std::uint32_t group{0}; ///< A2-04 offscreen group (0 = direct)
    };
    static_assert(std::is_trivially_copyable_v<AddImage2DPayload>);

    struct Image2DSlotReply
    {
        Image2DHandle handle{};
        ECanvas2DCreateStatus status{ECanvas2DCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<Image2DSlotReply>);

    struct LUX_OP(lane = frame, kind = stream, name = RemoveImage2D, method = removeImageRaw) RemoveImage2DPayload
    {
        RenderSceneId scene{};
        Image2DHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveImage2DPayload>);

    /// Per-frame transform delta (BulkData). Each entry self-routes by its own
    /// `scene` (G-04); only DIRTY images produce entries, so wire traffic is
    /// proportional to change, never to scene size.
    struct LUX_OP(lane = frame, kind = bulk, name = Image2DTransformBatch, method = updateTransformsRaw)
        Image2DTransformEntry
    {
        RenderSceneId scene{};
        Image2DHandle handle{};
        float m[6]{}; ///< column-major 2D affine (see Image2DInstanceData)
        std::int32_t page_delta[2]{};
    };
    static_assert(std::is_trivially_copyable_v<Image2DTransformEntry>);

    /// Visual fields (uv rect / tint / texture) — low-frequency, order-neutral.
    struct LUX_OP(lane = frame, kind = stream, name = UpdateImage2DVisual, method = updateVisualRaw)
        UpdateImage2DVisualPayload
    {
        RenderSceneId scene{};
        Image2DHandle handle{};
        float uv[4]{0.f, 0.f, 1.f, 1.f};
        std::uint32_t tint{0xFFFFFFFFu};
        std::uint32_t texture_bindless{kNoTexture};
    };
    static_assert(std::is_trivially_copyable_v<UpdateImage2DVisualPayload>);

    /// Order-affecting fields — the ONLY op that can trigger an order rebuild.
    struct LUX_OP(lane = frame, kind = stream, name = UpdateImage2DKey, method = updateKeyRaw) UpdateImage2DKeyPayload
    {
        RenderSceneId scene{};
        Image2DHandle handle{};
        float priority{0.f};
        std::uint32_t visible{1};
        std::uint32_t group{0}; ///< A2-04 offscreen group (0 = direct)
    };
    static_assert(std::is_trivially_copyable_v<UpdateImage2DKeyPayload>);

    /// Scene-level draw gate (retained bit). The camera bridge flips it when the
    /// publishable-camera gate changes — the concern ("never draw against a
    /// stale camera") expressed as ONE retained bit instead of per-frame liveness.
    struct LUX_OP(lane = frame, kind = stream, name = SetCanvas2DEnabled, method = setEnabledRaw)
        SetCanvas2DEnabledPayload
    {
        RenderSceneId scene{};
        std::uint32_t enabled{1};
    };
    static_assert(std::is_trivially_copyable_v<SetCanvas2DEnabledPayload>);

    // ── PixelField kind payloads (F2-09; same command grammar as images) ─────

    struct LUX_OP(
        lane = frame,
        kind = stream,
        name = AddPixelField2D,
        method = addPixelFieldRaw,
        reply = PixelFieldSlotReply) AddPixelField2DPayload
    {
        RenderSceneId scene{};
        PixelField2DInstanceData data{};
        float priority{0.f};
        std::uint32_t visible{1};
    };
    static_assert(std::is_trivially_copyable_v<AddPixelField2DPayload>);

    struct PixelFieldSlotReply
    {
        PixelFieldInstanceHandle handle{};
        ECanvas2DCreateStatus status{ECanvas2DCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<PixelFieldSlotReply>);

    struct LUX_OP(lane = frame, kind = stream, name = RemovePixelField2D, method = removePixelFieldRaw)
        RemovePixelField2DPayload
    {
        RenderSceneId scene{};
        PixelFieldInstanceHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<RemovePixelField2DPayload>);

    /// Fields are FEW (a handful per scene) and mostly static — a plain Stream op
    /// per moved field beats a bulk lane here.
    struct LUX_OP(
        lane = frame,
        kind = stream,
        name = UpdatePixelField2DTransform,
        method = updatePixelFieldTransformRaw) UpdatePixelField2DTransformPayload
    {
        RenderSceneId scene{};
        PixelFieldInstanceHandle handle{};
        float m[6]{};
        std::int32_t page_delta[2]{};
    };
    static_assert(std::is_trivially_copyable_v<UpdatePixelField2DTransformPayload>);

    struct LUX_OP(lane = frame, kind = stream, name = UpdatePixelField2DKey, method = updatePixelFieldKeyRaw)
        UpdatePixelField2DKeyPayload
    {
        RenderSceneId scene{};
        PixelFieldInstanceHandle handle{};
        float priority{0.f};
        std::uint32_t visible{1};
    };
    static_assert(std::is_trivially_copyable_v<UpdatePixelField2DKeyPayload>);

    // ── Tile kind payloads (A2-02; same command grammar as images/fields) ────

    struct LUX_OP(lane = frame, kind = stream, name = AddTile2D, method = addTilemapRaw, reply = Tile2DSlotReply)
        AddTile2DPayload
    {
        RenderSceneId scene{};
        Tile2DInstanceData data{};
        float priority{0.f};
        std::uint32_t visible{1};
    };
    static_assert(std::is_trivially_copyable_v<AddTile2DPayload>);

    struct Tile2DSlotReply
    {
        Tile2DInstanceHandle handle{};
        ECanvas2DCreateStatus status{ECanvas2DCreateStatus::Unknown};
    };
    static_assert(std::is_trivially_copyable_v<Tile2DSlotReply>);

    struct LUX_OP(lane = frame, kind = stream, name = RemoveTile2D, method = removeTilemapRaw) RemoveTile2DPayload
    {
        RenderSceneId scene{};
        Tile2DInstanceHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveTile2DPayload>);

    /// Tilemaps are FEW (a handful per scene) and mostly static — plain Stream
    /// ops per moved map, like fields.
    struct LUX_OP(lane = frame, kind = stream, name = UpdateTile2DTransform, method = updateTilemapTransformRaw)
        UpdateTile2DTransformPayload
    {
        RenderSceneId scene{};
        Tile2DInstanceHandle handle{};
        float m[6]{};
        std::int32_t page_delta[2]{};
    };
    static_assert(std::is_trivially_copyable_v<UpdateTile2DTransformPayload>);

    struct LUX_OP(lane = frame, kind = stream, name = UpdateTile2DKey, method = updateTilemapKeyRaw)
        UpdateTile2DKeyPayload
    {
        RenderSceneId scene{};
        Tile2DInstanceHandle handle{};
        float priority{0.f};
        std::uint32_t visible{1};
    };

} // namespace lux::render

// ════════════════════════════════════════════════════════════════════════
//  便捷面 —— 同名自由函数(三分法的第二类:带真语义,不是纯机械转发)
// ════════════════════════════════════════════════════════════════════════
//
//  生成的 Canvas2DProxy 方法收 Payload(*Raw 后缀)。下面这些替调用方把散参
//  打包成 Payload、做矩阵拷贝、填默认值 —— 这类"有语义"的便捷层按既有约定
//  手写成自由函数(与 MeshStack 的 addMeshInstance/updateTransforms 同款):
//  proxy 按值传首参,ADL 找得到。
//
//  声明在此(作者头),定义在 assembly/canvas2d/Canvas2DOperationHandlers.cpp。

namespace lux::render
{
    class Canvas2DProxy;                       // 生成于 Canvas2DOperation.ops.hpp
    template <typename T> class RenderRequest; // 前置声明:便捷面只按值返回它

    // ── Image ──
    [[nodiscard]] LUX_FUNCTION_PUBLIC RenderRequest<Image2DSlotReply> addImage(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        const Image2DInstanceData& data,
        float priority,
        bool visible = true,
        std::uint32_t group = 0
    );

    LUX_FUNCTION_PUBLIC void removeImage(Canvas2DProxy proxy, RenderSceneId scene, Image2DHandle handle);

    /// 逐条自带 scene 的异构批次(转发生成面)。
    LUX_FUNCTION_PUBLIC void updateTransforms(Canvas2DProxy proxy, std::span<const Image2DTransformEntry> entries);

    /// scene 盖章批量:让单场景批次不可能写漏 scene(与上面同名成对)。
    LUX_FUNCTION_PUBLIC void
    updateTransforms(Canvas2DProxy proxy, RenderSceneId scene, std::span<Image2DTransformEntry> entries);

    LUX_FUNCTION_PUBLIC void updateTransform(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Image2DHandle handle,
        const float m[6],
        const std::int32_t page_delta[2]
    );

    LUX_FUNCTION_PUBLIC void updateVisual(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Image2DHandle handle,
        const float uv[4],
        std::uint32_t tint,
        std::uint32_t texture_bindless
    );

    LUX_FUNCTION_PUBLIC void updateKey(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Image2DHandle handle,
        float priority,
        bool visible,
        std::uint32_t group = 0
    );

    LUX_FUNCTION_PUBLIC void setEnabled(Canvas2DProxy proxy, RenderSceneId scene, bool enabled);

    // ── PixelField ──
    [[nodiscard]] LUX_FUNCTION_PUBLIC RenderRequest<PixelFieldSlotReply> addPixelField(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        const PixelField2DInstanceData& data,
        float priority,
        bool visible = true
    );

    LUX_FUNCTION_PUBLIC void
    removePixelField(Canvas2DProxy proxy, RenderSceneId scene, PixelFieldInstanceHandle handle);

    LUX_FUNCTION_PUBLIC void updatePixelFieldTransform(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        PixelFieldInstanceHandle handle,
        const float m[6],
        const std::int32_t page_delta[2]
    );

    LUX_FUNCTION_PUBLIC void updatePixelFieldKey(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        PixelFieldInstanceHandle handle,
        float priority,
        bool visible
    );

    // ── Tilemap ──
    [[nodiscard]] LUX_FUNCTION_PUBLIC RenderRequest<Tile2DSlotReply> addTilemap(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        const Tile2DInstanceData& data,
        float priority,
        bool visible = true
    );

    LUX_FUNCTION_PUBLIC void removeTilemap(Canvas2DProxy proxy, RenderSceneId scene, Tile2DInstanceHandle handle);

    LUX_FUNCTION_PUBLIC void updateTilemapTransform(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Tile2DInstanceHandle handle,
        const float m[6],
        const std::int32_t page_delta[2]
    );

    LUX_FUNCTION_PUBLIC void updateTilemapKey(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Tile2DInstanceHandle handle,
        float priority,
        bool visible
    );

} // namespace lux::render
