#ifndef GBUFFER_ENCODE_SLANG
#define GBUFFER_ENCODE_SLANG
// =========================================================================
//  GBufferEncode.slang — shared GBuffer RT1 (normal + shading-model + rough)
//  pack/unpack SSOT.
//
//  RT1 (GBufNormalRoughness, RGBA16_SFLOAT) layout:
//    .xy = octahedral-encoded world normal  (frees .z vs the old raw xyz)
//    .z  = shading-model id (0=Unlit, 1=PBR), stored as an exact
//          small float (fp16 represents 0/1 exactly; read back via round)
//    .w  = roughness
//
//  Octahedral encoding maps a unit vector to [-1,1]^2; at fp16 (~11 mantissa
//  bits) this is at least as accurate as the previous raw 3x-fp16 normal, at
//  zero extra bandwidth — it frees the third channel for the shading-model id.
//
//  The material compiler writes this encoding; compatible lighting features decode it.
//  Resource placement is resolved by the RenderGraph compiler, not this include.
// =========================================================================

// Dense per-pixel lighting-model ids carried through the GBuffer. These are
// NOT the EShadingModel enum values (0/200/300/...) — they are a small dense
// tag chosen for the deferred dispatch switch. Keep in lock-step with the
// gbuffer writers and the emitter.
#define LUX_SM_UNLIT 0u
#define LUX_SM_PBR   1u

float2 octSignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral unit-vector encode/decode (Cigolle et al. 2014, "A Survey of
// Efficient Representations for Independent Unit Vectors").
float2 octEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 p = n.xy;
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * octSignNotZero(p)) : p;
}

float3 octDecode(float2 e)
{
    float3 v = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * octSignNotZero(v.xy);
    return normalize(v);
}

// RT1 pack/unpack: (oct.xy, shadingModelId, roughness).
float4 packGNormalSM(float3 worldNormal, uint shadingModelId, float roughness)
{
    return float4(octEncode(normalize(worldNormal)), float(shadingModelId), roughness);
}

void unpackGNormalSM(float4 g, out float3 worldNormal, out uint shadingModelId, out float roughness)
{
    worldNormal    = octDecode(g.xy);
    shadingModelId = uint(g.z + 0.5);
    roughness      = g.w;
}

#endif // GBUFFER_ENCODE_SLANG
