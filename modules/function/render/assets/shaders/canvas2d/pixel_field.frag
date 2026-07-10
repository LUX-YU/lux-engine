#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Canvas2D pixel-field fragment shader (F2-09: material-id mirror + palette).
//
// The field texture is R16_UNORM: each texel holds a MaterialId encoded as
// id/65535 — 16-bit integers round-trip EXACTLY through the float sampler, so
// `round(v * 65535)` recovers the id with no usampler binding in the global
// bindless set 2. texelFetch (no filtering, exact texel) keeps neighbouring
// ids from bleeding — the "nearest sampling" acceptance is by construction.
//
// The id indexes a 256×1 RGBA8 palette texture (PREMULTIPLIED colours; entry 0
// = the empty material = transparent black, so empty cells blend to nothing —
// no discard needed under ONE / ONE_MINUS_SRC_ALPHA). Material colour changes
// update ONLY the palette texture. Ids ≥ 256 clamp to 255 (the MVP palette
// width; a wider palette is a texture-size knob, not a shader change).

layout(set = 2, binding = 0) uniform sampler2D uTex[];

layout(location = 0) in vec2  vCell;
layout(location = 1) in vec4  vColor;
layout(location = 2) flat in uint vFieldTex;
layout(location = 3) flat in uint vPaletteTex;
layout(location = 4) flat in uvec2 vCells;
layout(location = 5) flat in uvec2 vAtlas;   // chunk texel origin in the atlas (C2-01)

layout(location = 0) out vec4 outColor;

void main()
{
    if (vFieldTex == 0xFFFFFFFFu || vPaletteTex == 0xFFFFFFFFu)
    {
        outColor = vec4(0.0);   // mirror/palette not resolved yet → draw nothing
        return;
    }

    const ivec2 cell = ivec2(clamp(ivec2(floor(vCell)),
                                   ivec2(0), ivec2(vCells) - ivec2(1)));
    const float v  = texelFetch(uTex[nonuniformEXT(vFieldTex)],
                                ivec2(vAtlas) + cell, 0).r;
    const uint  id = min(uint(round(v * 65535.0)), 255u);
    const vec4  c  = texelFetch(uTex[nonuniformEXT(vPaletteTex)], ivec2(int(id), 0), 0);
    outColor = c * vColor;      // palette is premultiplied; entry 0 is transparent
}
