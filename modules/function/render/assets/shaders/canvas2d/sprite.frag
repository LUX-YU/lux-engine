#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Canvas2D sprite fragment shader (S2-02: bindless-textured, premultiplied).
//
// Each sprite carries its own bindless texture index (per-vertex, flat). A sentinel
// 0xFFFFFFFF means "no texture" → the flat premultiplied tint (so an untextured sprite
// needs no default/white texture). Otherwise sample the bindless combined-sampler set 2
// (the domain-neutral TextureResources atlas — the same set the 3D material path uses)
// at the sprite's UV.
//
// The pass blends PREMULTIPLIED (ONE / ONE_MINUS_SRC_ALPHA), so the fragment output must
// be premultiplied. Uploaded textures are STRAIGHT-alpha (the engine's texture path does
// not pre-multiply; typical PNG), so we pre-multiply the sample HERE (rgb *= a) before
// modulating by the (already-premultiplied) tint — without this a semi-transparent texel
// haloes / double-darkens. (A future per-sprite Canvas2DAlphaMode could skip this for an
// already-premultiplied atlas.)

layout(set = 2, binding = 0) uniform sampler2D uTex[];

layout(location = 0) in vec2  vUV;
layout(location = 1) in vec4  vColor;
layout(location = 2) flat in uint vTexBindless;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vTexBindless == 0xFFFFFFFFu)
    {
        outColor = vColor;   // tint-only (already premultiplied)
    }
    else
    {
        vec4 t = texture(uTex[nonuniformEXT(vTexBindless)], vUV);
        t.rgb *= t.a;        // straight-alpha sample → premultiplied
        outColor = t * vColor;
    }
}
