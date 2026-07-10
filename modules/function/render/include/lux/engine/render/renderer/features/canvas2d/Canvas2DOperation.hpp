#pragma once
// ============================================================================
//  Canvas2DOperation.hpp — the public protocol between the 2D renderable bridges
//  (gameplay d2) and the render-side Canvas2DFeature (SinglePerScene).
//
//  v2 (C2D-R0, GPU-DRIVEN — .internal/2d-gpu-driven-rewrite.md): sprites are
//  GPU-RESIDENT instances in the scene's Canvas2D instance arena, maintained by
//  create / destroy / delta commands (the MeshStack paradigm). This header holds
//  the comm-free half: the instance record (std430 mirror), the owner handle,
//  the priority quantization and the create status. The ops themselves live in
//  Canvas2DFeatureOps.hpp.
//
//  DRAW-ORDER CONTRACT (frozen, decision ① 2026-07-06):
//   - every live+visible instance has key = (quantizePriority(priority) << 32)
//     | slot_index; the canvas draws keys in ASCENDING order;
//   - HIGHER priority ⇒ HIGHER key ⇒ drawn LATER ⇒ ON TOP;
//   - equal priorities tie-break by slot index (≈ creation order) — the key
//     space is a STRICT TOTAL ORDER by construction, deterministically;
//   - the key space is shared by every future canvas producer (tile chunks /
//     pixel-field chunks interleave with sprites on the same axis).
//   - blend stays premultiplied alpha (Canvas2DAlphaMode).
// ============================================================================

#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey (generational owner handles)

#include <bit>
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// Blend contract: premultiplied alpha (kept as an enum so a future
    /// straight-alpha path is a value, not a rewrite).
    enum class Canvas2DAlphaMode : std::uint8_t
    {
        Premultiplied = 0,
        Straight      = 1,
    };

    /// Sentinel bindless index meaning "no texture — draw the flat premultiplied tint".
    /// (The bindless allocator can hand out index 0 for a real texture, so 0 can't be the
    /// sentinel.) The sprite fragment shader branches on it, so an untextured sprite needs
    /// no default/white texture. Producers set a real index (RTextureHandle::index) once a
    /// texture resolves; the default leaves the draw tint-only.
    inline constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;

    // ════════════════════════════════════════════════════════════════════════
    //  v2 GPU-driven instance protocol (C2D-R0)
    // ════════════════════════════════════════════════════════════════════════

    struct Sprite2DInstanceTag{};
    /// Generational per-sprite owner handle into the scene's Canvas2D instance
    /// arena. A stale handle (generation mismatch) is rejected by every op.
    using Sprite2DHandle = lux::cxx::SlotKey<Sprite2DInstanceTag>;

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
    static_assert(quantizePriority(-1e30f)  < quantizePriority(-1.0f));
    static_assert(quantizePriority(-1.0f)   < quantizePriority(-0.5f));
    static_assert(quantizePriority(-0.5f)   < quantizePriority(-0.0f));
    static_assert(quantizePriority(-0.0f)   < quantizePriority(0.0f));
    static_assert(quantizePriority(0.0f)    < quantizePriority(0.5f));
    static_assert(quantizePriority(0.5f)    < quantizePriority(1.0f));
    static_assert(quantizePriority(1.0f)    < quantizePriority(1e30f));

    /// GPU-resident per-sprite record — the std430 mirror the sprite vertex
    /// shader pulls (keep canvas2d/sprite.vert in sync). Scalar fields only, so
    /// C++ packing == std430 packing (48 B, array stride 48 in an SSBO).
    struct Sprite2DInstanceData
    {
        /// 2D affine world transform, column-major: [c0.x c0.y c1.x c1.y tx ty].
        /// The producer bakes size×pivot in; the VS expands a unit ±0.5 quad.
        float m[6]{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
        float uv[4]{0.f, 0.f, 1.f, 1.f};              ///< atlas rect: u0, v0, w, h
        std::uint32_t tint{0xFFFFFFFFu};              ///< premultiplied RGBA8
        std::uint32_t texture_bindless{kNoTexture};   ///< set-2 index / kNoTexture = tint-only
    };
    static_assert(sizeof(Sprite2DInstanceData) == 48,
                  "Sprite2DInstanceData layout drift — keep canvas2d/sprite.vert in sync.");
    static_assert(std::is_trivially_copyable_v<Sprite2DInstanceData>);

    // ── PixelField kind (F2-09) ─────────────────────────────────────────────

    struct PixelFieldInstanceTag{};
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
        float m[6]{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};    ///< column-major 2D affine (this CHUNK's extent baked in)
        std::uint32_t field_texture{kNoTexture};      ///< bindless set-2 index of the R16_UNORM id mirror (C2-01: the scene ATLAS)
        std::uint32_t palette_texture{kNoTexture};    ///< bindless set-2 index of the 256×1 RGBA8 palette
        std::uint32_t cells_w{0};                     ///< this chunk's texel extent (texelFetch bounds)
        std::uint32_t cells_h{0};
        std::uint32_t tint{0xFFFFFFFFu};              ///< premultiplied RGBA8 modulate
        /// C2-01: the chunk's texel origin inside the atlas texture (0,0 for a
        /// dedicated texture). texelFetch reads atlas_origin + cell.
        std::uint32_t atlas_x{0};
        std::uint32_t atlas_y{0};
        std::uint32_t _pad0{0};
    };
    static_assert(sizeof(PixelField2DInstanceData) == 56,
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
    struct Canvas2DCommConfig
    {
        std::uint32_t offscreen_groups{0};   ///< clamped to kMaxCanvas2DGroups
    };
    static_assert(std::is_trivially_copyable_v<Canvas2DCommConfig>);

    // ── Tile kind (A2-02) ───────────────────────────────────────────────────

    struct Tile2DInstanceTag{};
    /// Generational per-tilemap owner handle into the canvas arena's Tile kind
    /// store. A stale handle is rejected by every op.
    using Tile2DInstanceHandle = lux::cxx::SlotKey<Tile2DInstanceTag>;

    /// Tile-id texel meaning "no tile here" in the R16_UNORM index texture
    /// (encoded 0xFFFF/65535 = 1.0 — the max unorm value, unreachable as a
    /// real tile ordinal since tileset ids are < cols*rows ≤ 0xFFFF).
    inline constexpr std::uint16_t kEmptyTile = 0xFFFFu;

    /// GPU-resident per-TILEMAP record — the std430 mirror the tile shaders
    /// pull (keep canvas2d/tile.vert in sync). One instance = one whole
    /// tilemap quad (the PixelField shape): the fragment shader texelFetches
    /// the R16_UNORM tile-INDEX texture, derives the tile's uv rect from the
    /// tileset's uniform grid (cols × rows, no margin/spacing in the MVP) and
    /// samples the tileset texture. Editing tiles = a region update of the
    /// index texture — the instance record never changes.
    struct Tile2DInstanceData
    {
        float m[6]{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};    ///< column-major 2D affine (full map extent baked in)
        std::uint32_t tileset_texture{kNoTexture};    ///< bindless set-2 index of the tileset atlas
        std::uint32_t index_texture{kNoTexture};      ///< bindless set-2 index of the R16_UNORM tile-id map
        std::uint32_t tiles_w{0};                     ///< index-map texel extent (texelFetch bounds)
        std::uint32_t tiles_h{0};
        std::uint32_t tileset_grid{0};                ///< packed: cols (low 16) | rows (high 16)
        std::uint32_t tint{0xFFFFFFFFu};              ///< premultiplied RGBA8 modulate
    };
    static_assert(sizeof(Tile2DInstanceData) == 48,
                  "Tile2DInstanceData layout drift — keep canvas2d/tile.vert in sync.");
    static_assert(std::is_trivially_copyable_v<Tile2DInstanceData>);

    [[nodiscard]] constexpr std::uint32_t packTilesetGrid(std::uint32_t cols, std::uint32_t rows) noexcept
    {
        return (cols & 0xFFFFu) | (rows << 16);
    }

    /// addSprite outcome (G-05 discipline, mirrors MeshInstanceCreateStatus):
    /// anything but Ok ⇒ NO instance exists (handle is null) — the client must
    /// not treat it as live. `Unknown` is the DEFAULT on purpose: a generic
    /// dispatch failure delivers a default-constructed reply the server never
    /// filled, so the default must be neither Ok (silent zombie) nor a retriable
    /// error (endless retry). Only CapacityExhausted is transient.
    enum class ECanvas2DCreateStatus : std::uint32_t
    {
        Unknown              = 0,
        Ok                   = 1,
        InvalidConfiguration = 2,   ///< scene / Canvas2DFeature absent — permanent
        CapacityExhausted    = 3,   ///< arena at max capacity — transient
    };

} // namespace lux::render
