#pragma once
// ============================================================================
//  TilemapComponent.hpp — a uniform-grid tilemap (A2-02, lux::pack).
//
//  The component OWNS the runtime tile grid (u16 ordinals, 2 B/tile — small
//  enough for ECS storage, unlike a pixel field's cell planes) plus the
//  tileset parameters (inlined: the GPU path needs only a uniform cols×rows
//  grid — a separate shareable TilesetAsset is deliberately deferred, see the
//  A2-02 log entry). The retained Tilemap2DBridge mirrors the grid into a
//  persistent R16_UNORM index texture and one canvas Tile instance; EDITS go
//  through setTile()/fill() which bump `revision` and grow the dirty rect, so
//  a local edit uploads a local region — never the whole map.
//
//  Bookkeeping contract: `revision` + the dirty rect are BRIDGE-CONSUMED state
//  (the G-07 "system-owned derived field" convention): the bridge resets the
//  rect after a successfully acknowledged upload of that revision. Gameplay
//  code must edit ONLY through the helpers; writing `tiles` raw and skipping
//  the helpers is the one unsupported path (the value-compare axiom cannot
//  cover a 2 B/tile grid without a per-frame full memcmp).
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/asset/Asset.hpp>   // lux::asset::asset_id_t
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>   // kEmptyTile

#include <algorithm>
#include <cstdint>
#include <vector>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    using lux::render::kEmptyTile;   // 0xFFFF — "no tile here"

    struct LUX_COMPONENT() TilemapComponent
    {
        /// The tileset atlas TEXTURE asset (uniform cols×rows grid of tiles,
        /// ordinals row-major with row 0 at the TOP of the image).
        LUX_MEMBER(display_name=Tileset, tooltip=Tileset atlas texture asset)
        lux::asset::asset_id_t tileset_texture{};

        LUX_MEMBER(display_name=Tileset Cols, tooltip=Tiles per tileset row)
        std::uint32_t tileset_cols{1};

        LUX_MEMBER(display_name=Tileset Rows, tooltip=Tileset rows)
        std::uint32_t tileset_rows{1};

        /// World units per tile (the map spans tiles_w×tile_size world units).
        LUX_MEMBER(display_name=Tile Size, tooltip=World units per tile)
        float tile_size{0.1f};

        LUX_MEMBER(display_name=Priority, tooltip=Draw priority; higher is on top)
        float priority{0.f};

        LUX_MEMBER(display_name=Visible, tooltip=Whether the map is drawn)
        bool visible{true};

        LUX_MEMBER(display_name=Tint, tooltip=Premultiplied RGBA8 tint)
        std::uint32_t tint{0xFFFFFFFFu};

        // ── the grid (runtime state; row-major, row 0 = BOTTOM, F2-02 style) ──
        std::uint32_t              tiles_w{0};
        std::uint32_t              tiles_h{0};
        std::vector<std::uint16_t> tiles;   ///< tiles_w*tiles_h ordinals; kEmptyTile = hole

        // ── bridge-consumed upload bookkeeping ──
        std::uint64_t revision{0};          ///< bumped by every edit helper
        std::int32_t  dirty_min_x{0}, dirty_min_y{0};
        std::int32_t  dirty_max_x{-1}, dirty_max_y{-1};   ///< inclusive; max<min = clean

        [[nodiscard]] bool hasDirty() const noexcept
        { return dirty_max_x >= dirty_min_x && dirty_max_y >= dirty_min_y; }

        void clearDirty() noexcept
        { dirty_min_x = dirty_min_y = 0; dirty_max_x = dirty_max_y = -1; }

        /// (Re)size the grid, filling with @p fill_id, and mark everything dirty.
        void resize(std::uint32_t w, std::uint32_t h, std::uint16_t fill_id = kEmptyTile)
        {
            tiles_w = w;
            tiles_h = h;
            tiles.assign(static_cast<std::size_t>(w) * h, fill_id);
            ++revision;
            dirty_min_x = 0; dirty_min_y = 0;
            dirty_max_x = static_cast<std::int32_t>(w) - 1;
            dirty_max_y = static_cast<std::int32_t>(h) - 1;
        }

        /// The ONLY supported single-tile edit path (bumps revision + dirty rect).
        void setTile(std::uint32_t x, std::uint32_t y, std::uint16_t id)
        {
            if (x >= tiles_w || y >= tiles_h) return;
            auto& t = tiles[static_cast<std::size_t>(y) * tiles_w + x];
            if (t == id) return;
            t = id;
            ++revision;
            growDirty(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                      static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
        }

        [[nodiscard]] std::uint16_t tileAt(std::uint32_t x, std::uint32_t y) const noexcept
        {
            return (x < tiles_w && y < tiles_h)
                 ? tiles[static_cast<std::size_t>(y) * tiles_w + x] : kEmptyTile;
        }

        /// Rect fill (clamped), one revision bump.
        void fill(std::int32_t x0, std::int32_t y0, std::int32_t w, std::int32_t h,
                  std::uint16_t id)
        {
            const std::int32_t x1 = std::min<std::int32_t>(x0 + w, static_cast<std::int32_t>(tiles_w)) - 1;
            const std::int32_t y1 = std::min<std::int32_t>(y0 + h, static_cast<std::int32_t>(tiles_h)) - 1;
            const std::int32_t cx0 = std::max(x0, 0), cy0 = std::max(y0, 0);
            if (cx0 > x1 || cy0 > y1) return;
            for (std::int32_t y = cy0; y <= y1; ++y)
                for (std::int32_t x = cx0; x <= x1; ++x)
                    tiles[static_cast<std::size_t>(y) * tiles_w + x] = id;
            ++revision;
            growDirty(cx0, cy0, x1, y1);
        }

    private:
        void growDirty(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) noexcept
        {
            if (!hasDirty())
            {
                dirty_min_x = x0; dirty_min_y = y0;
                dirty_max_x = x1; dirty_max_y = y1;
                return;
            }
            dirty_min_x = std::min(dirty_min_x, x0);
            dirty_min_y = std::min(dirty_min_y, y0);
            dirty_max_x = std::max(dirty_max_x, x1);
            dirty_max_y = std::max(dirty_max_y, y1);
        }
    };

} // namespace lux::pack
