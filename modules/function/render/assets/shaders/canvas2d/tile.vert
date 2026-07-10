#version 450

// =============================================================================
// Canvas2D tile vertex shader (A2-02, GPU-driven — vertex pulling).
//
// One instance = one WHOLE tilemap quad (the pixel-field shape). The varying
// carries the TILE-space position; the fragment shader texelFetches the
// R16_UNORM tile-index map and samples the tileset atlas.
//
// Tile2DInstanceData (std430, 48 B — keep Canvas2DOperation.hpp in sync):
//   float m[6]       column-major 2D affine: c0.x c0.y c1.x c1.y tx ty
//   uint  tileset    bindless set-2 index of the tileset atlas texture
//   uint  index_tex  bindless set-2 index of the R16_UNORM tile-id map
//   uint  tiles_w/h  index-map texel extent
//   uint  grid       packed tileset grid: cols (low 16) | rows (high 16)
//   uint  tint       premultiplied RGBA8 modulate
// =============================================================================

struct Tile2DInstance {
    float m0x; float m0y;
    float m1x; float m1y;
    float tx;  float ty;
    uint  tileset_tex;
    uint  index_tex;
    uint  tiles_w;
    uint  tiles_h;
    uint  grid;
    uint  tint;
};

layout(set = 1, binding = 0, std430) readonly buffer TileBuf {
    Tile2DInstance tiles[];
} uTiles;

layout(set = 1, binding = 1, std430) readonly buffer OrderBuf {
    uint order[];
} uOrder;

struct ViewGpuData {
    mat4 view;
    mat4 proj;
    mat4 inv_view;
    mat4 inv_proj;
    vec4 cam_pos;
    vec4 viewport;
};
layout(set = 0, binding = 1, std430) readonly buffer ViewBuffer {
    ViewGpuData views[];
} uViews;

layout(push_constant) uniform PC {
    uint scene_index;
    uint view_index;
} uPC;

layout(location = 0) out vec2 vTile;          // tile-space position (0..tiles_w/h)
layout(location = 1) out vec4 vColor;         // premultiplied tint
layout(location = 2) flat out uint vTilesetTex;
layout(location = 3) flat out uint vIndexTex;
layout(location = 4) flat out uvec2 vTiles;   // texelFetch clamp bounds
layout(location = 5) flat out uvec2 vGrid;    // tileset cols / rows

const vec2 kCorners[6] = vec2[6](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5,  0.5), vec2(-0.5, 0.5)
);

void main()
{
    const uint slot = uOrder.order[gl_InstanceIndex];
    const Tile2DInstance t = uTiles.tiles[slot];
    const vec2 c = kCorners[gl_VertexIndex];

    const vec2 world = vec2(t.m0x * c.x + t.m1x * c.y + t.tx,
                            t.m0y * c.x + t.m1y * c.y + t.ty);
    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * vec4(world, 0.0, 1.0);

    // tile (0,0) is the map's BOTTOM-LEFT (+y up), same contract as the
    // pixel field — corner (-0.5,-0.5) maps to tile (0,0).
    vTile       = (c + vec2(0.5, 0.5)) * vec2(float(t.tiles_w), float(t.tiles_h));
    vTilesetTex = t.tileset_tex;
    vIndexTex   = t.index_tex;
    vTiles      = uvec2(t.tiles_w, t.tiles_h);
    vGrid       = uvec2(t.grid & 0xFFFFu, t.grid >> 16u);

    const float r = float((t.tint >>  0u) & 0xFFu) / 255.0;
    const float g = float((t.tint >>  8u) & 0xFFu) / 255.0;
    const float b = float((t.tint >> 16u) & 0xFFu) / 255.0;
    const float a = float((t.tint >> 24u) & 0xFFu) / 255.0;
    vColor = vec4(r, g, b, a);
}
