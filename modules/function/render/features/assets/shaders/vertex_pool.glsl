#ifndef VERTEX_POOL_GLSL
#define VERTEX_POOL_GLSL
// =========================================================================
//  vertex_pool.glsl — bindless vertex source array (descriptor set 7)
//
//  Lets mesh vertex shaders read geometry from any registered IVertexSource
//  uniformly, with no per-kind branching:
//
//    MeshVertex v = luxFetchMeshVertex(prop.vertex_pool_id,
//                                      prop.vertex_base, gl_VertexIndex);
//
//  Pool id 0 is conventionally the static global VBO (StaticVertexSource);
//  ids 1+ are transient compute-written pools (skinning, morph, cloth, ...).
//
//  Layout: each pool is a flat float SSBO. The vertex stride + per-attribute
//  float offsets are NOT hardwired here — they arrive as specialization
//  constants (see vertex_layout.glsl), so one shader binary serves any
//  registered VertexLayout. Defaults equal the legacy lux::rdesc::Vertex
//  layout (22 floats / 88 B: pos, normal, tangent, uv, bitangent, then the
//  bone tail). Skinned pools are fed the lean layout 1 (14 floats / 56 B,
//  geometry only). Mesh vertex shaders read only the geometry prefix; any
//  bone tail is consumed by the skinning compute kernel, never here.
//
//  std430 float[] has a 4-byte stride (no padding), so a flat float array
//  reproduces the tightly-packed C++ struct exactly without needing the
//  scalar-block-layout extension. This is why we read scalars rather than
//  declaring a vec3-bearing struct (which std430 would 16-byte-pad).
//
//  Requires GL_EXT_nonuniform_qualifier: vertex_pool_id comes from the
//  per-instance InstanceProperty and is therefore potentially non-uniform
//  across the invocations of a single (instanced) draw.
// =========================================================================
#extension GL_EXT_nonuniform_qualifier : require

// The bindless vertex-pool array lives at descriptor set 7 for graphics mesh
// shaders (the EDescriptorSetSlot::VertexPool contract). The skinning compute
// kernel binds the SAME descriptor set at a different index (its pipeline
// layout has no set 7), so it #defines LUX_VERTEX_POOL_SET before including.
#ifndef LUX_VERTEX_POOL_SET
#define LUX_VERTEX_POOL_SET 7
#endif

layout(set = LUX_VERTEX_POOL_SET, binding = 0, std430) readonly buffer VertexPool {
    float data[];
} luxVertexPools[];

// Stride + per-attribute float offsets come from the layout contract (set
// per-pipeline via spec constants; defaults == the legacy 22-float layout).
#include "vertex_layout.glsl"

// Back-compat alias: the skinning compute references LUX_VERTEX_FLOAT_STRIDE for
// its INPUT stride (the READ layout).
#define LUX_VERTEX_FLOAT_STRIDE LUX_VTX_FLOAT_STRIDE

struct MeshVertex {
    vec3 position;
    vec3 normal;
    vec3 tangent;
    vec2 uv;
    vec3 bitangent;
};

// Reads a geometry MeshVertex from the READ layout (110-block). The _vp mesh
// shaders pass the skinned-output pool here.
MeshVertex luxFetchMeshVertex(uint pool_id, uint vertex_base, uint vertex_index)
{
    uint p = nonuniformEXT(pool_id);
    uint o = (vertex_base + vertex_index) * LUX_VTX_FLOAT_STRIDE;
    MeshVertex v;
    v.position  = vec3(luxVertexPools[p].data[o + LUX_VTX_OFF_POSITION + 0u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_POSITION + 1u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_POSITION + 2u]);
    v.normal    = vec3(luxVertexPools[p].data[o + LUX_VTX_OFF_NORMAL + 0u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_NORMAL + 1u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_NORMAL + 2u]);
    v.tangent   = vec3(luxVertexPools[p].data[o + LUX_VTX_OFF_TANGENT + 0u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_TANGENT + 1u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_TANGENT + 2u]);
    v.uv        = vec2(luxVertexPools[p].data[o + LUX_VTX_OFF_UV0 + 0u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_UV0 + 1u]);
    v.bitangent = vec3(luxVertexPools[p].data[o + LUX_VTX_OFF_BITANGENT + 0u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_BITANGENT + 1u],
                       luxVertexPools[p].data[o + LUX_VTX_OFF_BITANGENT + 2u]);
    return v;
}
#endif // VERTEX_POOL_GLSL
