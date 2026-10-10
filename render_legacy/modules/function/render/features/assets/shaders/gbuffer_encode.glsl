#ifndef GBUFFER_ENCODE_GLSL
#define GBUFFER_ENCODE_GLSL
// =========================================================================
//  gbuffer_encode.glsl — shared GBuffer RT1 (normal + shading-model + rough)
//  pack/unpack SSOT.
//
//  RT1 (GBufNormalRoughness, RGBA16_SFLOAT) layout:
//    .xy = octahedral-encoded world normal  (frees .z vs the old raw xyz)
//    .z  = shading-model id (0=Unlit, 1=PBR, 2=Toon), stored as an exact
//          small float (fp16 represents 0/1/2 exactly; read back via round)
//    .w  = roughness
//
//  Octahedral encoding maps a unit vector to [-1,1]^2; at fp16 (~11 mantissa
//  bits) this is at least as accurate as the previous raw 3x-fp16 normal, at
//  zero extra bandwidth — it frees the third channel for the shading-model id.
//
//  LOCK-STEP CONTRACT: every GBuffer producer (gbuffer_{pbr,unlit,stylized}.frag
//  AND the material-graph emitter's gbuffer epilogue) MUST write RT1 via
//  packGNormalSM(); the only consumer (deferred_lighting.frag) MUST read it via
//  unpackGNormalSM(). The emitter is device-free (no #include resolver) so it
//  keeps an inline copy of these helpers — keep the two in sync.
// =========================================================================

// Dense per-pixel lighting-model ids carried through the GBuffer. These are
// NOT the EShadingModel enum values (0/200/300/...) — they are a small dense
// tag chosen for the deferred dispatch switch. Keep in lock-step with the
// gbuffer writers and the emitter.
#define LUX_SM_UNLIT 0u
#define LUX_SM_PBR   1u
#define LUX_SM_TOON  2u

vec2 octSignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral unit-vector encode/decode (Cigolle et al. 2014, "A Survey of
// Efficient Representations for Independent Unit Vectors").
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 p = n.xy;
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * octSignNotZero(p)) : p;
}

vec3 octDecode(vec2 e)
{
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * octSignNotZero(v.xy);
    return normalize(v);
}

// RT1 pack/unpack: (oct.xy, shadingModelId, roughness).
vec4 packGNormalSM(vec3 worldNormal, uint shadingModelId, float roughness)
{
    return vec4(octEncode(normalize(worldNormal)), float(shadingModelId), roughness);
}

void unpackGNormalSM(vec4 g, out vec3 worldNormal, out uint shadingModelId, out float roughness)
{
    worldNormal    = octDecode(g.xy);
    shadingModelId = uint(g.z + 0.5);
    roughness      = g.w;
}

#endif // GBUFFER_ENCODE_GLSL
