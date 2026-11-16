#include "../Common.slang"
#ifdef PAGED_INSTANCES
uint64_t luxAddress(uint2 words)
{
    return uint64_t(words.x) | (uint64_t(words.y) << 32);
}
#endif
#ifdef PAGED_INSTANCES
#endif

struct Transform
{
    mat4 model;
};

struct Word
{
    uint value;
};

struct Property
{
    uint mesh_index;
    uint material_buffer;
    uint material_record;
    uint section_index;
    uint render_flags;
};
#ifdef PAGED_INSTANCES
struct Address
{
    uvec2 value;
};

struct PageAddresses
{
    uvec2 transforms;
    uvec2 properties;
    uvec2 alive;
};

[[vk::binding(0, 0)]] StructuredBuffer<Address> transforms : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<Address> alive : register(t1, space0);
[[vk::binding(7, 0)]] StructuredBuffer<Address> properties : register(t7, space0);
#else
[[vk::binding(0, 0)]] StructuredBuffer<Transform> transforms : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<Word> alive : register(t1, space0);
[[vk::binding(7, 0)]] StructuredBuffer<Property> properties : register(t7, space0);
#endif
struct View
{
    mat4 world_to_clip;
    vec4 planes[6];
    uvec4 flags;
};

[[vk::binding(2, 0)]] StructuredBuffer<View> views : register(t2, space0);

struct Lux_candidates
{
    uint value;
};

[[vk::binding(3, 0)]] StructuredBuffer<Lux_candidates> candidates : register(t3, space0);

// Records/chunk indices address the dense live-group table, not recycled CPU logical slots.
struct DrawGroup
{
    uint index_count;
    uint candidate_count;
    uint first_index;
    int vertex_offset;
    uint candidate_offset;
    uint visible_offset;
    uint visible_stride;
    uint view_count;
    vec4 center;
    vec4 half_extent;
};

[[vk::binding(4, 0)]] StructuredBuffer<DrawGroup> groups : register(t4, space0);

struct Lux_chunks
{
    uvec2 value;
};

[[vk::binding(6, 0)]] StructuredBuffer<Lux_chunks> chunks : register(t6, space0);

struct Lux_visible
{
    uint value;
};

[[vk::binding(5, 0)]] RWStructuredBuffer<Lux_visible> visible : register(u5, space0);

struct Parameters
{
    uint frustum_culling;
    uint shadow_pass;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

groupshared uvec2 prefix[64];
groupshared uvec2 first;

void luxMain()
{
    uint view = gl_WorkGroupID.y;
    uvec2 chunk = chunks[gl_WorkGroupID.x].value;
    DrawGroup group = groups[chunk.x];
    uint candidate = chunk.y + gl_LocalInvocationIndex;
    uint lane = gl_LocalInvocationIndex;
    uint instance = 0;
    uint winding = 0;
    bool included = candidate < group.candidate_count && views[view].flags.x != 0;
    if (included)
    {
        instance = candidates[group.candidate_offset + candidate].value;
#ifdef PAGED_INSTANCES
        uint root = instance >> 23;
        uint leaf = (instance >> 14) & 511;
        uint slot = instance & 16383;
        mat4 model =
            ((Transform*)luxAddress(((PageAddresses*)luxAddress(transforms[root].value))[leaf].transforms))[slot].model;
        uint present = ((Word*)luxAddress(((PageAddresses*)luxAddress(alive[root].value))[leaf].alive))[slot].value;
        uint flags =
            ((Property*)luxAddress(((PageAddresses*)luxAddress(properties[root].value))[leaf].properties))[slot]
                .render_flags;
#else
        mat4 model = transforms[instance].model;
        uint present = alive[instance].value;
        uint flags = properties[instance].render_flags;
#endif
        float determinant = dot(transpose(model)[0].xyz, cross(transpose(model)[1].xyz, transpose(model)[2].xyz));
        winding = determinant < 0 ? 1 : 0;
        uint required = parameters.shadow_pass != 0 ? 3u : 1u;
        included = present != 0 && determinant != 0 && (flags & required) == required;
        if (included && parameters.frustum_culling != 0 && group.center.w != 0)
        {
            // Legacy frustumCull half-space test. The support radius uses the transformed AABB axes,
            // rather than a uniform-scale sphere assumption: mirrors, shear and nonuniform scales stay conservative.
            vec3 center = (mul(model, vec4(group.center.xyz, 1))).xyz;
            for (uint p = 0; p < 6; ++p)
            {
                vec4 plane = views[view].planes[p];
                vec3 local_normal = mul(transpose((float3x3)model), plane.xyz);
                float radius = dot(abs(local_normal), group.half_extent.xyz);
                if (dot(plane.xyz, center) + plane.w < -radius)
                {
                    included = false;
                }
            }
        }
    }
    prefix[lane] = uvec2(included && winding == 0 ? 1 : 0, included && winding == 1 ? 1 : 0);
    barrier();
    // One contiguous reservation per workgroup, as in Legacy's shadow cull. All lanes reach every barrier.
    for (uint stride = 1; stride < 64; stride <<= 1)
    {
        uvec2 before = lane >= stride ? prefix[lane - stride] : uvec2(0);
        barrier();
        prefix[lane] += before;
        barrier();
    }
    uint stride = group.visible_stride;
    uint offset = group.visible_offset + view * 2 * stride;
    if (lane == 63)
    {
        first.x = atomicAdd(visible[offset + 1].value, prefix[lane].x);
        first.y = atomicAdd(visible[offset + stride + 1].value, prefix[lane].y);
    }
    barrier();
    if (included)
    {
        visible[offset + winding * stride + 5 + first[winding] + prefix[lane][winding] - 1].value = instance;
    }
}

[shader("compute")][numthreads(64, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
