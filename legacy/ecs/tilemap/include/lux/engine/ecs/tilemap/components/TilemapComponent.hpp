#pragma once

#include <lux/engine/ecs/tilemap/TilemapId.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <cstdint>

namespace lux::ecs
{
    /// Authored tilemap facts. Runtime storage and handles live in a transient
    /// binding component, so representation state never enters cooked data.
    struct LUX_COMPONENT() TilemapComponent final
    {
        TilemapId LUX_NO_MEMBER() id{};

        LUX_MEMBER(display_name=Tileset, asset_type=texture, tooltip=Tileset texture asset)
        lux::asset::asset_id_t tileset_texture{};

        LUX_MEMBER(display_name=Tileset Cols, tooltip=Tiles per tileset row)
        std::uint32_t tileset_cols{1u};

        LUX_MEMBER(display_name=Tileset Rows, tooltip=Tileset rows)
        std::uint32_t tileset_rows{1u};

        LUX_MEMBER(display_name=Tile Size, tooltip=World units per tile)
        float tile_size{0.1f};

        LUX_MEMBER(display_name=Priority, tooltip=Draw priority; higher is on top)
        float priority{0.0f};

        LUX_MEMBER(display_name=Visible, tooltip=Whether the map is drawn)
        bool visible{true};

        LUX_MEMBER(display_name=Tint, tooltip=Premultiplied RGBA8 tint)
        std::uint32_t tint{0xFFFFFFFFu};
    };
} // namespace lux::ecs
