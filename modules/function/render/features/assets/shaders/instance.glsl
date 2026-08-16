// =========================================================================
//  instance.glsl — Three-stream instance data (std430)
//
//  Must match C++ structs in InstanceResources.hpp:
//    InstanceTransform (64 B), InstanceCullMeta (64 B),
//    InstanceProperty  (80 B).
//  (以 C++ 侧的 static_assert 为准 —— 此前这里写的 32 B 已与之不符。)
// =========================================================================
#ifndef INSTANCE_GLSL
#define INSTANCE_GLSL

#ifdef LUX_INSTANCE_PAGED
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

struct InstanceTransform {
    vec4  basis_local_x;      // xyz = basis column 0, w = local x
    vec4  basis_local_y;      // xyz = basis column 1, w = local y
    vec4  basis_local_z;      // xyz = basis column 2, w = local z
    ivec3 page_delta;         // object page relative to RenderScene origin page
    uint  flags;
};

mat3 luxSpatialBasis(InstanceTransform transform) {
    return mat3(
        transform.basis_local_x.xyz,
        transform.basis_local_y.xyz,
        transform.basis_local_z.xyz
    );
}

vec3 luxSpatialLocal(InstanceTransform transform) {
    return vec3(
        transform.basis_local_x.w,
        transform.basis_local_y.w,
        transform.basis_local_z.w
    );
}

vec3 luxSpatialTranslation(
    InstanceTransform transform,
    ivec3 view_page_delta,
    vec3 view_local,
    float coordinate_page_size
) {
    return vec3(transform.page_delta - view_page_delta) * coordinate_page_size
         + luxSpatialLocal(transform) - view_local;
}

mat4 luxSpatialModel(
    InstanceTransform transform,
    ivec3 view_page_delta,
    vec3 view_local,
    float coordinate_page_size
) {
    mat3 basis = luxSpatialBasis(transform);
    vec3 translation = luxSpatialTranslation(
        transform,
        view_page_delta,
        view_local,
        coordinate_page_size
    );
    return mat4(
        vec4(basis[0], 0.0),
        vec4(basis[1], 0.0),
        vec4(basis[2], 0.0),
        vec4(translation, 1.0)
    );
}

struct InstanceCullMeta {
    vec4  bsphere;            // xyz = normalized page-local center, w = radius
    ivec4 bsphere_page;       // center page relative to RenderScene origin
    uint  bucket_id;          //  4B
    uint  lod_count;          //  4B  valid lod_mdc[] entries (0 = unregistered)
    uint  lod_mdc[4];         // 16B  MDC index per LOD level (std430 stride 4)
    uint  _pad0;              //  4B
    uint  _pad1;              //  4B   total = 64 bytes
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
    float transition_start_time;
    float transition_duration;
    uint  transition_seed;
    uint  transition_flags;   // bit0 active, bit1 fade-out ─ 64 B ─
    uint  rgba8;
    uint  _property_pad0;
    uint  _property_pad1;
    uint  _property_pad2;     // ─ 80 B ─
};

struct GpuVisibleInstance {
    uint instance_slot;
    uint slice_index;
};

#ifdef LUX_INSTANCE_PAGED
struct GpuInstancePageAddresses {
    uint64_t transform;
    uint64_t previous_transform;
    uint64_t property;
    uint64_t cull_meta;
};

layout(buffer_reference, std430, buffer_reference_align = 8)
readonly buffer InstancePageLeafRef {
    GpuInstancePageAddresses pages[];
};
layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer InstanceTransformPageRef {
    InstanceTransform values[];
};
layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer InstancePropertyPageRef {
    InstanceProperty values[];
};
layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer InstanceCullMetaPageRef {
    InstanceCullMeta values[];
};

const uint LUX_INSTANCE_PAGE_OFFSET_BITS = 14u;
const uint LUX_INSTANCE_LEAF_BITS = 9u;
const uint LUX_INSTANCE_PAGE_OFFSET_MASK = (1u << 14u) - 1u;

uint luxInstanceRootIndex(uint slot) {
    return slot >> (LUX_INSTANCE_PAGE_OFFSET_BITS + LUX_INSTANCE_LEAF_BITS);
}

GpuInstancePageAddresses luxInstancePage(
    uint64_t leaf_address,
    uint slot
) {
    InstancePageLeafRef leaf = InstancePageLeafRef(leaf_address);
    uint page_index = slot >> LUX_INSTANCE_PAGE_OFFSET_BITS;
    return leaf.pages[page_index & ((1u << LUX_INSTANCE_LEAF_BITS) - 1u)];
}

InstanceTransform luxLoadInstanceTransform(
    uint64_t leaf_address,
    uint slot
) {
    GpuInstancePageAddresses page = luxInstancePage(leaf_address, slot);
    return InstanceTransformPageRef(page.transform).values[
        slot & LUX_INSTANCE_PAGE_OFFSET_MASK];
}

InstanceProperty luxLoadInstanceProperty(
    uint64_t leaf_address,
    uint slot
) {
    GpuInstancePageAddresses page = luxInstancePage(leaf_address, slot);
    return InstancePropertyPageRef(page.property).values[
        slot & LUX_INSTANCE_PAGE_OFFSET_MASK];
}

InstanceCullMeta luxLoadInstanceCullMeta(
    uint64_t leaf_address,
    uint slot
) {
    GpuInstancePageAddresses page = luxInstancePage(leaf_address, slot);
    return InstanceCullMetaPageRef(page.cull_meta).values[
        slot & LUX_INSTANCE_PAGE_OFFSET_MASK];
}
#endif

const uint LUX_TRANSITION_ACTIVE = 1u;
const uint LUX_TRANSITION_FADE_OUT = 2u;

float luxTransitionCoverage(InstanceProperty prop, float scene_time) {
    if ((prop.transition_flags & LUX_TRANSITION_ACTIVE) == 0u)
        return 1.0;
    float progress = clamp(
        (scene_time - prop.transition_start_time) /
            max(prop.transition_duration, 1e-5),
        0.0,
        1.0
    );
    return (prop.transition_flags & LUX_TRANSITION_FADE_OUT) != 0u
        ? 1.0 - progress
        : progress;
}

/// Return material_index directly — full 32-bit range.
uint packMaterialIndex(InstanceProperty prop) {
    return prop.material_index;
}

// One VkDrawIndexedIndirectCommand, as written by the cull kernel and compacted
// by mdc_compact. The field order and types are the Vulkan struct's, so this is
// pinned on the C++ side by kIndirectCommandSize == 20 ==
// sizeof(VkDrawIndexedIndirectCommand) — see ShadowKernels.cpp's use of the real
// sizeof. Lives here rather than in each kernel because both already include
// this header, and a second hand-copy is how ShadowSliceGPU drifted.
struct IndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

#endif // INSTANCE_GLSL
