#version 450
#ifdef PAGED_INSTANCES
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#endif
layout(location=0) in vec3 position;
#ifndef DEPTH_ONLY
layout(location=1) in vec2 uv;
layout(location=3) out vec2 vUV;
layout(location=4) out flat uint vMatIndex;
layout(location=7) out flat float vTransitionCoverage;
layout(location=8) out flat uint vTransitionSeed;
layout(location=9) out flat uint vTransitionFadeOut;
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
    vUV = uv;
    vMatIndex = property.material_index;
    vTransitionCoverage = 1;
    vTransitionSeed = 0;
    vTransitionFadeOut = 0;
#endif
}
