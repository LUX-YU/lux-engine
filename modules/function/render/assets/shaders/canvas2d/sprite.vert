#version 450

// =============================================================================
// Canvas2D sprite vertex shader (v2, GPU-driven — vertex pulling).
//
// NO vertex inputs. Each instance is one sprite: gl_InstanceIndex walks the
// arena's ORDER buffer (slot indices in ascending sort-key order — the draw
// sequence IS the painter order), the slot indexes the instance records, and
// gl_VertexIndex (0..5) synthesizes a unit ±0.5 quad corner that the sprite's
// baked 2D affine places in world space. The scene camera comes from the SAME
// per-view ViewGpuData path as 3D (set 0, binding 1).
//
// Sprite2DInstanceData (std430, 48 B — keep Canvas2DOperation.hpp in sync):
//   float m[6]     column-major 2D affine: c0.x c0.y c1.x c1.y tx ty
//   float uv[4]    atlas rect: u0, v0, w, h
//   uint  tint     PREMULTIPLIED RGBA8, R[7:0]..A[31:24]
//   uint  tex      bindless set-2 index, or 0xFFFFFFFF = tint-only
// =============================================================================

struct Sprite2DInstance {
    float m0x; float m0y;   // world col0.xy
    float m1x; float m1y;   // world col1.xy
    float tx;  float ty;    // translation
    float u0;  float v0;    // uv origin
    float uw;  float vh;    // uv extent
    uint  tint;
    uint  tex;
};

layout(set = 1, binding = 0, std430) readonly buffer SpriteBuf {
    Sprite2DInstance sprites[];
} uSprites;

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

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out uint vTexBindless;

// Two CCW triangles over a unit quad centred at the origin.
const vec2 kCorners[6] = vec2[6](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5,  0.5), vec2(-0.5, 0.5)
);

void main()
{
    const uint slot = uOrder.order[gl_InstanceIndex];
    const Sprite2DInstance s = uSprites.sprites[slot];
    const vec2 c = kCorners[gl_VertexIndex];

    const vec2 world = vec2(s.m0x * c.x + s.m1x * c.y + s.tx,
                            s.m0y * c.x + s.m1y * c.y + s.ty);
    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * vec4(world, 0.0, 1.0);

    // uv: +x → u grows; +y (up) → v shrinks (top of the sprite = top of the rect).
    vUV          = vec2(s.u0 + (c.x + 0.5) * s.uw,
                        s.v0 + (0.5 - c.y) * s.vh);
    vTexBindless = s.tex;

    const float r = float((s.tint >>  0u) & 0xFFu) / 255.0;
    const float g = float((s.tint >>  8u) & 0xFFu) / 255.0;
    const float b = float((s.tint >> 16u) & 0xFFu) / 255.0;
    const float a = float((s.tint >> 24u) & 0xFFu) / 255.0;
    vColor = vec4(r, g, b, a);   // already premultiplied (pipeline blends ONE / 1-SRC_ALPHA)
}
