#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Canvas2D tile fragment shader (A2-02: tile-index map + tileset atlas).
//
// The index map is R16_UNORM: each texel holds a tile ordinal encoded as
// id/65535 (exact round-trip through the float sampler — the pixel-field
// trick); texelFetch keeps neighbouring ids from bleeding. 0xFFFF is the
// EMPTY sentinel → transparent (blends to nothing under premultiplied ONE /
// ONE_MINUS_SRC_ALPHA, no discard).
//
// The ordinal maps onto the tileset's UNIFORM grid (cols × rows, row 0 at the
// TOP of the atlas image, ordinals row-major — the standard tileset layout;
// no margin/spacing in the MVP). Sampling insets by half an index-map texel
// scaled into the tile's uv rect, so linear filtering never reads the
// neighbouring tile's edge (bleed guard).

layout(set = 2, binding = 0) uniform sampler2D uTex[];

layout(location = 0) in vec2  vTile;
layout(location = 1) in vec4  vColor;
layout(location = 2) flat in uint vTilesetTex;
layout(location = 3) flat in uint vIndexTex;
layout(location = 4) flat in uvec2 vTiles;
layout(location = 5) flat in uvec2 vGrid;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vTilesetTex == 0xFFFFFFFFu || vIndexTex == 0xFFFFFFFFu ||
        vGrid.x == 0u || vGrid.y == 0u)
    {
        outColor = vec4(0.0);   // not resolved yet → draw nothing
        return;
    }

    const ivec2 cell = ivec2(clamp(ivec2(floor(vTile)),
                                   ivec2(0), ivec2(vTiles) - ivec2(1)));
    const float v  = texelFetch(uTex[nonuniformEXT(vIndexTex)], cell, 0).r;
    const uint  id = uint(round(v * 65535.0));
    if (id == 0xFFFFu)          // empty tile sentinel
    {
        outColor = vec4(0.0);
        return;
    }

    const uint col = id % vGrid.x;
    const uint row = id / vGrid.x;
    if (row >= vGrid.y)         // ordinal beyond the tileset → content error, draw nothing
    {
        outColor = vec4(0.0);
        return;
    }

    // Position inside THIS tile in [0,1)². Inset by half a texel of the
    // tileset tile so linear filtering cannot bleed the neighbouring tile.
    const vec2 tile_size = vec2(1.0) / vec2(vGrid);
    vec2 local = fract(vTile);
    const vec2 half_texel = 0.5 / vec2(textureSize(uTex[nonuniformEXT(vTilesetTex)], 0));
    const vec2 lo = half_texel / tile_size;
    local = clamp(local, lo, vec2(1.0) - lo);

    // Atlas v: ordinal rows run top-down while tile-space +y runs up — flip.
    const vec2 uv = (vec2(col, row) + vec2(local.x, 1.0 - local.y)) * tile_size;
    const vec4 texel = texture(uTex[nonuniformEXT(vTilesetTex)], uv);
    // Tileset atlases are straight-alpha authored art — premultiply to match
    // the canvas blend contract (same as the sprite path).
    outColor = vec4(texel.rgb * texel.a, texel.a) * vColor;
}
