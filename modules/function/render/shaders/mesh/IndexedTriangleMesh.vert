#version 450
#ifdef PAGED_INSTANCES
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#endif
layout(location=0) in vec3 position;
#ifndef DEPTH_ONLY
#ifndef VERTEX_NO_UV
layout(location=1) in vec2 uv;
layout(location=3) out vec2 vUV;
#endif
layout(location=0) out vec3 vWorldPos;
#ifdef VERTEX_NORMALS
layout(location=2) in vec3 normal;
layout(location=1) out vec3 vWorldNormal;
#endif
#ifdef VERTEX_TANGENTS
layout(location=3) in vec4 tangent;
layout(location=5) out vec4 vWorldTangent;
layout(location=6) out vec3 vWorldBitangent;
#endif
layout(location=4) out flat uint vMatIndex;
#endif
struct Transform { mat4 model; };
struct Property { uint mesh_index; uint material_index; uint section_index; };
struct Word { uint value; };
#ifdef PAGED_INSTANCES
struct Address { uvec2 value; };
struct PageAddresses { uvec2 transforms; uvec2 properties; uvec2 alive; };
layout(buffer_reference, std430, buffer_reference_align=8) readonly buffer PageLeaf { PageAddresses pages[]; };
layout(buffer_reference, std430, buffer_reference_align=16) readonly buffer TransformPage { Transform records[]; };
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer PropertyPage { Property records[]; };
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer AlivePage { Word records[]; };
layout(set=3,binding=7,std430) readonly buffer TransformRoots { Address roots[]; } transforms;
layout(set=3,binding=8,std430) readonly buffer PropertyRoots { Address roots[]; } properties;
layout(set=3,binding=9,std430) readonly buffer AliveRoots { Address roots[]; } alive;
#else
layout(set=3,binding=7,std430) readonly buffer Transforms { Transform records[]; } transforms;
layout(set=3,binding=8,std430) readonly buffer Properties { Property records[]; } properties;
layout(set=3,binding=9,std430) readonly buffer Alive { Word records[]; } alive;
#endif
layout(set=2,binding=6,std430) readonly buffer Members { Word words[]; } members;
layout(push_constant) uniform Camera { mat4 view; mat4 projection; } camera;
void main()
{
    uint slot = members.words[5 + gl_InstanceIndex].value;
#ifdef PAGED_INSTANCES
    uint root = slot >> 23;
    uint leaf = (slot >> 14) & 511;
    uint local = slot & 16383;
    mat4 model = TransformPage(PageLeaf(transforms.roots[root].value).pages[leaf].transforms).records[local].model;
    Property property = PropertyPage(PageLeaf(properties.roots[root].value).pages[leaf].properties).records[local];
    uint present = AlivePage(PageLeaf(alive.roots[root].value).pages[leaf].alive).records[local].value;
#else
    mat4 model = transforms.records[slot].model;
    Property property = properties.records[slot];
    uint present = alive.records[slot].value;
#endif
    gl_Position = present != 0 ? camera.projection * camera.view * model * vec4(position, 1) : vec4(2, 2, 2, 1);
#ifndef DEPTH_ONLY
#ifndef VERTEX_NO_UV
    vUV = uv;
#endif
    vWorldPos = (model * vec4(position, 1.0)).xyz;
#ifdef VERTEX_NORMALS
    mat3 linear = mat3(model);
    // Legacy cofactor construction, corrected for negative determinants and tangent-vector transforms.
    mat3 cofactor = mat3(cross(linear[1], linear[2]), cross(linear[2], linear[0]), cross(linear[0], linear[1]));
    float handedness = sign(dot(linear[0], cofactor[0]));
    vec3 n = normalize(cofactor * normal * handedness);
    vWorldNormal = n;
#ifdef VERTEX_TANGENTS
    vec3 t = linear * tangent.xyz;
    t = normalize(t - n * dot(n, t));
    float orientation = tangent.w * handedness;
    vWorldTangent = vec4(t, orientation);
    vWorldBitangent = cross(n, t) * orientation;
#endif
#endif
    vMatIndex = property.material_index;
#endif
}
