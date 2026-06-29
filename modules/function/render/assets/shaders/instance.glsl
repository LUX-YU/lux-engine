// =========================================================================
//  instance.glsl — Three-stream instance data (std430)
//
//  Must match C++ structs in InstanceResources.hpp:
//    InstanceTransform (64 B), InstanceCullMeta (32 B),
//    InstanceProperty  (32 B).
// =========================================================================
#ifndef INSTANCE_GLSL
#define INSTANCE_GLSL

struct InstanceTransform {
    mat4 world;               // 64 bytes
};

struct InstanceCullMeta {
    vec4  bsphere;            // xyz = center (world), w = radius (<0 = tombstone)
    uint  bucket_id;          //  4B
    uint  lod_count;          //  4B  valid lod_mdc[] entries (0 = unregistered)
    uint  lod_mdc[4];         // 16B  MDC index per LOD level (std430 stride 4)
    uint  _pad0;              //  4B
    uint  _pad1;              //  4B   total = 48 bytes
};

struct MeshSectionRecord {
    uint first_index;
    uint index_count;
    int  base_vertex;
    uint vertex_count;
};

struct InstanceProperty {
    uint  object_id;          //  4B
    uint  layer_mask;         //  4B
    uint  flags;              //  4B
    // Canonical pack contract: family_id must match shading_model_id family.
    uint  material_type;      //  4B  (family_id << 12) | shading_model_id
    uint  material_index;     //  4B
    uint  transform_index;    //  4B  normally == slot index
    uint  pass_and_geometry;  // low16 pass_mask, next8 geometry_kind
    uint  user_meta_index;    //  4B  ─── 32 byte mark ───
    // Vertex source fields. ~0u in vertex_pool_id means "no bindless source
    // registered" (legacy fallback; not used by current pipelines since every
    // mesh now reads through the bindless pool).
    uint  vertex_pool_id;     //  4B
    uint  vertex_base;        //  4B  honest output base (no CPU bias)
    uint  vertex_count;       //  4B
    uint  input_vertex_offset;//  4B  _vp gl_VertexIndex bias == draw vertexOffset ─ 48 B ─
};

/// Return material_index directly — full 32-bit range.
uint packMaterialIndex(InstanceProperty prop) {
    return prop.material_index;
}

#endif // INSTANCE_GLSL
