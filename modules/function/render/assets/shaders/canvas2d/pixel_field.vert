#version 450

// =============================================================================
// Canvas2D pixel-field vertex shader (F2-09, GPU-driven — vertex pulling).
//
// One instance = one field chunk quad. gl_InstanceIndex walks the PixelField
// kind's ORDER buffer, the slot indexes the instance records, gl_VertexIndex
// synthesizes a unit ±0.5 quad corner placed by the record's baked affine
// (the FULL field extent is baked into the scale). The varying carries the
// CELL-space position for the fragment shader's exact texelFetch.
//
// PixelField2DInstanceData (std430, 48 B — keep Canvas2DOperation.hpp in sync):
//   float m[6]      column-major 2D affine: c0.x c0.y c1.x c1.y tx ty
//   uint  field     bindless set-2 index of the R16_UNORM material-id mirror
//   uint  palette   bindless set-2 index of the 256×1 RGBA8 palette
//   uint  cells_w/h id-mirror texel extent
//   uint  tint      premultiplied RGBA8 modulate
// =============================================================================

struct PixelField2DInstance {
    float m0x; float m0y;
    float m1x; float m1y;
    float tx;  float ty;
    uint  field_tex;
    uint  palette_tex;
    uint  cells_w;
    uint  cells_h;
    uint  tint;
    uint  atlas_x;
    uint  atlas_y;
    uint  _pad0;
};

layout(set = 1, binding = 0, std430) readonly buffer FieldBuf {
    PixelField2DInstance fields[];
} uFields;

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

layout(location = 0) out vec2 vCell;          // cell-space position (0..cells_w/h)
layout(location = 1) out vec4 vColor;         // premultiplied tint
layout(location = 2) flat out uint vFieldTex;
layout(location = 3) flat out uint vPaletteTex;
layout(location = 4) flat out uvec2 vCells;   // texelFetch clamp bounds
layout(location = 5) flat out uvec2 vAtlas;   // chunk texel origin in the atlas (C2-01)

const vec2 kCorners[6] = vec2[6](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5,  0.5), vec2(-0.5, 0.5)
);

void main()
{
    const uint slot = uOrder.order[gl_InstanceIndex];
    const PixelField2DInstance f = uFields.fields[slot];
    const vec2 c = kCorners[gl_VertexIndex];

    const vec2 world = vec2(f.m0x * c.x + f.m1x * c.y + f.tx,
                            f.m0y * c.x + f.m1y * c.y + f.ty);
    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * vec4(world, 0.0, 1.0);

    // cell (0,0) is the field's BOTTOM-LEFT (+y up), and cell row r is uploaded
    // to texture row r — corner (-0.5,-0.5) therefore maps to cell (0,0).
    vCell       = (c + vec2(0.5, 0.5)) * vec2(float(f.cells_w), float(f.cells_h));
    vFieldTex   = f.field_tex;
    vPaletteTex = f.palette_tex;
    vCells      = uvec2(f.cells_w, f.cells_h);
    vAtlas      = uvec2(f.atlas_x, f.atlas_y);

    const float r = float((f.tint >>  0u) & 0xFFu) / 255.0;
    const float g = float((f.tint >>  8u) & 0xFFu) / 255.0;
    const float b = float((f.tint >> 16u) & 0xFFu) / 255.0;
    const float a = float((f.tint >> 24u) & 0xFFu) / 255.0;
    vColor = vec4(r, g, b, a);
}
