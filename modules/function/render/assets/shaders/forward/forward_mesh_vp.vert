#version 450
// =========================================================================
//  forward_mesh_vp.vert — forward vertex shader, VERTEX-POOL variant
//
//  Vertices come from the bindless vertex pool array (set 7) instead of
//  vertex attribute inputs. Used by every mesh draw — static meshes
//  register the global VBO as a pool entry; skinned meshes write to a
//  transient pool consumed here as well.
//
//  No vertex attribute inputs: gl_VertexIndex + InstanceProperty's
//  vertex_pool_id / vertex_base resolve the vertex.
// =========================================================================

// ========== SET 0: View Buffer ==========
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

layout(push_constant) uniform SharedPC {
    uint scene_index;
    uint view_index;
} uPC;

// ========== SET 1: Three-stream instance data ==========
#include "instance.glsl"

layout(set = 1, binding = 0, std430) readonly buffer TransformBuf {
    InstanceTransform transforms[];
} uTransforms;

layout(set = 1, binding = 1, std430) readonly buffer PropertyBuf {
    InstanceProperty properties[];
} uProperties;

layout(set = 5, binding = 0, std430) readonly buffer VisibleInstanceBuf {
    uint ids[];
} uVisibleInstances;

// ========== SET 7: Bindless vertex pools ==========
#include "vertex_pool.glsl"

// ========== Varyings (matches family forward fragment interface) ==========
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 2) out vec3 vViewDir;
layout(location = 3) out vec2 vUV;
layout(location = 4) out flat uint vMatIndex;
layout(location = 5) out vec3 vWorldTangent;
layout(location = 6) out vec3 vWorldBitangent;

void main()
{
    uint visibleIndex = uint(gl_InstanceIndex);
    uint drawSlot = uVisibleInstances.ids[visibleIndex];

    InstanceTransform xform = uTransforms.transforms[drawSlot];
    InstanceProperty  prop  = uProperties.properties[drawSlot];

    // gl_VertexIndex == draw vertexOffset + index == input_vertex_offset + local.
    // Subtract the bias so vertex_base stays the honest output base,
    // instead of the old CPU-side (out_base - in_base) uint-wraparound encoding.
    uint localVertex = uint(gl_VertexIndex) - prop.input_vertex_offset;
    MeshVertex mv = luxFetchMeshVertex(prop.vertex_pool_id,
                                       prop.vertex_base,
                                       localVertex);

    mat4 model = xform.world;
    // Cofactor (adjugate) of mat3(model) = det(M) * inverse-transpose; direction
    // matches and the normalize()s below cancel the missing 1/det (correct for
    // non-uniform scale) at ~1/3 the per-vertex cost. (P-3)
    mat3 _m    = mat3(model);
    mat3 Nmat  = mat3(cross(_m[1], _m[2]), cross(_m[2], _m[0]), cross(_m[0], _m[1]));

    vec4 worldPos    = model * vec4(mv.position, 1.0);
    vec3 worldNorm   = normalize(Nmat * mv.normal);
    vec3 worldTang   = normalize(Nmat * mv.tangent);
    vec3 worldBitang = normalize(Nmat * mv.bitangent);

    // Gram-Schmidt re-orthogonalise
    worldTang   = normalize(worldTang - dot(worldTang, worldNorm) * worldNorm);
    worldBitang = normalize(worldBitang
                  - dot(worldBitang, worldNorm) * worldNorm
                  - dot(worldBitang, worldTang) * worldTang);

    vWorldPos      = worldPos.xyz;
    vWorldNormal   = worldNorm;
    vWorldTangent  = worldTang;
    vWorldBitangent = worldBitang;
    vViewDir       = normalize(uViews.views[uPC.view_index].cam_pos.xyz - worldPos.xyz);
    vUV            = mv.uv;
    vMatIndex      = packMaterialIndex(prop);

    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * worldPos;
}
