#pragma once
// ============================================================================
//  Canvas2DOperation.hpp — the FROZEN public protocol between the 2D renderable
//  bridges (gameplay d2: Sprite2D / Tilemap2D / PixelField2D) and the render-side
//  Canvas2DFeature (SinglePerScene). R2-00 freezes the PODs; the feature + ops
//  (submit / handle CRUD) are R2-01.
//
//  Contract (design §3.3):
//   - Every draw carries a DrawOrderKey forming a STRICT TOTAL ORDER; the feature
//     does ONE stable sort → painter order. Duplicate keys are illegal (debug assert
//     in the feature).
//   - Draw packets are trivially-copyable value PODs — NO raw borrowed pointers that
//     would dangle across the frame. Variable-length producer data (tile chunk indices,
//     pixel chunk textures) is OWNED server-side and referenced by an owner handle
//     (SpriteBatchHandle / TilemapRenderHandle / PixelFieldRenderHandle) — NOT the
//     MeshStack RenderObjectHandle (a 2D batch is not a mesh instance).
//   - MVP blend is premultiplied alpha.
//   - The submit op (R2-01) carries the owning RenderSceneId explicitly (scene-routed,
//     like every other feature op — G-04).
// ============================================================================

#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey (generational owner handles)

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    // ── Owner handles (generational; NOT the MeshStack RenderObjectHandle) ──
    struct SpriteBatchTag{};
    struct TilemapRenderTag{};
    struct PixelFieldRenderTag{};
    using SpriteBatchHandle      = lux::cxx::SlotKey<SpriteBatchTag>;
    using TilemapRenderHandle    = lux::cxx::SlotKey<TilemapRenderTag>;
    using PixelFieldRenderHandle = lux::cxx::SlotKey<PixelFieldRenderTag>;

    /// MVP is premultiplied alpha; kept as an enum so a future straight-alpha path is a
    /// value, not a rewrite. (M0A protocol decision — design §3.3.)
    enum class Canvas2DAlphaMode : std::uint8_t
    {
        Premultiplied = 0,
        Straight      = 1,
    };

    /// Strict-total-order draw key (design §3.3). Ordered field-by-field
    /// layer < sublayer < order < producer_order < stable_id, so a single stable sort
    /// expresses e.g. `tile-bg < pixel-field < tile-fg < sprite < pixel-fg` — which
    /// coarse ERenderStage z-buckets cannot. `order` may pack a quantised y-sort;
    /// `producer_order` + `stable_id` are the deterministic final tie-breaks (no two
    /// distinct items ever compare equal in a well-formed frame).
    struct DrawOrderKey
    {
        std::int16_t  layer          = 0;
        std::uint16_t sublayer       = 0;
        std::int32_t  order          = 0;   ///< within-layer order (may encode y-sort)
        std::uint32_t producer_order = 0;   ///< which producer/bridge emitted it
        std::uint64_t stable_id      = 0;   ///< deterministic final tie-break (kills non-determinism)
    };
    static_assert(std::is_trivially_copyable_v<DrawOrderKey>);
    static_assert(std::is_standard_layout_v<DrawOrderKey>);

    [[nodiscard]] constexpr bool operator<(const DrawOrderKey& a, const DrawOrderKey& b) noexcept
    {
        if (a.layer          != b.layer)          return a.layer          < b.layer;
        if (a.sublayer       != b.sublayer)       return a.sublayer       < b.sublayer;
        if (a.order          != b.order)          return a.order          < b.order;
        if (a.producer_order != b.producer_order) return a.producer_order < b.producer_order;
        return a.stable_id < b.stable_id;
    }
    [[nodiscard]] constexpr bool operator==(const DrawOrderKey& a, const DrawOrderKey& b) noexcept
    {
        return a.layer == b.layer && a.sublayer == b.sublayer && a.order == b.order
            && a.producer_order == b.producer_order && a.stable_id == b.stable_id;
    }
    [[nodiscard]] constexpr bool operator!=(const DrawOrderKey& a, const DrawOrderKey& b) noexcept { return !(a == b); }

    /// A 2D rectangle (UV in an atlas, or a region in field space). POD.
    struct Rect2D
    {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    };
    static_assert(std::is_trivially_copyable_v<Rect2D>);

    /// Sentinel bindless index meaning "no texture — draw the flat premultiplied tint".
    /// (The bindless allocator can hand out index 0 for a real texture, so 0 can't be the
    /// sentinel.) The sprite fragment shader branches on it, so an untextured sprite needs
    /// no default/white texture. Producers set a real index (RTextureHandle::index) once a
    /// texture resolves; the default leaves the draw tint-only.
    inline constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;

    // ── Minimal render-side draw packets (typed lists; firmed up in R2-01) ──
    //  Each carries a column-major 4x4 world transform (the 2D pose embedded, matching
    //  the zero-copy std430 mat4 path) + its DrawOrderKey + a premultiplied RGBA8 tint.

    /// One textured sprite quad.
    struct SpriteDraw
    {
        DrawOrderKey  key{};
        float         transform[16]{};                ///< column-major world matrix (2D embedded)
        Rect2D        uv{};                            ///< uv rect in the atlas texture
        std::uint32_t texture_bindless = kNoTexture;  ///< bindless set-2 index, or kNoTexture (tint-only)
        std::uint32_t tint = 0xFFFFFFFFu;             ///< premultiplied RGBA8
    };
    static_assert(std::is_trivially_copyable_v<SpriteDraw>);

    /// One tile-chunk batch — its index grid is OWNED server-side (referenced by handle),
    /// never a borrowed pointer in the packet.
    struct TileDraw
    {
        DrawOrderKey        key{};
        float               transform[16]{};
        TilemapRenderHandle chunk{};          ///< server-owned chunk (index grid + dims)
        std::uint32_t       tileset_bindless = 0;
        std::uint32_t       tint = 0xFFFFFFFFu;
    };
    static_assert(std::is_trivially_copyable_v<TileDraw>);

    /// One pixel-field chunk quad — the chunk's GPU texture is OWNED server-side.
    struct PixelFieldDraw
    {
        DrawOrderKey           key{};
        float                  transform[16]{};
        PixelFieldRenderHandle field{};        ///< server-owned field (chunk texture cache)
        Rect2D                 region{};        ///< the chunk's region in field space
        std::uint32_t          texture_bindless = 0;
        std::uint32_t          tint = 0xFFFFFFFFu;
    };
    static_assert(std::is_trivially_copyable_v<PixelFieldDraw>);

} // namespace lux::render
