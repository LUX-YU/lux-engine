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
layout(location = 3) flat in vec4 vRect;   // atlas rect: u0, v0, uw, vh

layout(location = 0) out vec4 outColor;

void main()
{
    if (vTexBindless == 0xFFFFFFFFu)
    {
        outColor = vColor;   // tint-only (already premultiplied)
    }
    else
    {
        // ATLAS-SAFE sampling. The derivatives of vUV are honest (vUV is
        // continuous across the quad) — nothing here is mathematically wrong.
        // The DATA is: a generated mip chain averages across the WHOLE image,
        // so every mip>0 texel of an ATLAS mixes neighbouring frames, and
        // bilinear filtering at the rect's edge reaches into the next frame
        // (by half a texel OF THE SAMPLED LEVEL — 8 source pixels at mip 4).
        // A minified sprite therefore haloes with its neighbour's colour and,
        // at the 1x1 level, samples the whole-atlas average.
        //
        // Two bounds, both required:
        //  1. LOD ≤ log2(min rect extent) - 1 — never let one texel span more
        //     than half the sprite's own frame;
        //  2. sample point pulled inside the rect by half a texel AT THAT LOD
        //     — the same inset the tile shader applies at mip 0, scaled.
        // A whole-texture sprite (uv rect 0,0,1,1) loses only the last two
        // sub-pixel levels and a fractional-texel inset: no visible change.
        // Belt to the asset side's braces: pixel-art atlases should ALSO carry
        // ETextureAssetFlags::NO_MIPS, which removes the chain entirely.
        const vec2  tex_size = vec2(textureSize(uTex[nonuniformEXT(vTexBindless)], 0));
        const vec2  duvdx    = dFdx(vUV) * tex_size;
        const vec2  duvdy    = dFdy(vUV) * tex_size;
        const float lod_raw  = 0.5 * log2(max(dot(duvdx, duvdx), dot(duvdy, duvdy)));
        const float rect_texels = max(1.0, min(vRect.z * tex_size.x, vRect.w * tex_size.y));
        const float max_lod  = max(0.0, floor(log2(rect_texels)) - 1.0);
        const float lod      = clamp(lod_raw, 0.0, max_lod);

        // exp2(lod + 1): trilinear filtering blends the NEXT level too, whose
        // texels are twice as wide — inset for the coarser of the pair.
        const vec2 half_texel = 0.5 * exp2(lod + 1.0) / tex_size;
        const vec2 lo = vRect.xy + half_texel;
        const vec2 hi = vRect.xy + vRect.zw - half_texel;
        const vec2 uv = clamp(vUV, min(lo, hi), max(lo, hi));

        vec4 t = textureLod(uTex[nonuniformEXT(vTexBindless)], uv, lod);
        t.rgb *= t.a;        // straight-alpha sample → premultiplied
        outColor = t * vColor;
    }
}
