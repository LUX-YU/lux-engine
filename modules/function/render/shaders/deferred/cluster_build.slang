#include "../Common.slang"

struct ClusterBounds
{
    vec4 minimum;
    vec4 maximum;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<ClusterBounds> bounds : register(u0, space0);

struct Grid
{
    uvec4 cells;
    uvec4 limits;
    vec4 position_near;
    vec4 direction_far;
    mat4 world_to_clip;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<Grid> grid_data : register(u1, space0);

struct Status
{
    uvec4 values;
};

[[vk::binding(2, 0)]] RWStructuredBuffer<Status> status : register(u2, space0);

struct Parameters
{
    mat4 inverse_view_projection;
    vec4 position_near;
    vec4 direction_far;
    uvec4 cells;
    uvec4 limits;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

bool finiteVector(vec4 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

float sliceDistance(uint z)
{
    float t = float(z) / float(parameters.cells.z);
    float near_z = parameters.position_near.w, far_z = parameters.direction_far.w;
    return parameters.cells.w == 0 ? mix(near_z, far_z, t) : near_z * pow(far_z / near_z, t);
}

void luxMain()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= parameters.limits.x)
    {
        return;
    }
    uvec3 cell = uvec3(
        index % parameters.cells.x,
        (index / parameters.cells.x) % parameters.cells.y,
        index / (parameters.cells.x * parameters.cells.y)
    );
    bool valid = finiteVector(parameters.position_near) && finiteVector(parameters.direction_far) &&
                 parameters.position_near.w > 0 && parameters.direction_far.w > parameters.position_near.w &&
                 abs(dot(parameters.direction_far.xyz, parameters.direction_far.xyz) - 1) < 0.001;
    vec3 minimum = vec3(3.402823e38), maximum = -minimum;
    for (uint y = 0; y < 2; ++y)
    {
        for (uint x = 0; x < 2; ++x)
        {
            vec2 xy = vec2(cell.xy + uvec2(x, y)) / vec2(parameters.cells.xy) * 2 - 1;
            vec4 a = mul(parameters.inverse_view_projection, vec4(xy, 0, 1));
            vec4 b = mul(parameters.inverse_view_projection, vec4(xy, 1, 1));
            bool ray_valid = finiteVector(a) && finiteVector(b) && abs(a.w) > 1e-12 && abs(b.w) > 1e-12;
            if (!ray_valid)
            {
                valid = false;
                continue;
            }
            vec3 origin = a.xyz / a.w, ray = b.xyz / b.w - origin;
            float depth = dot(origin - parameters.position_near.xyz, parameters.direction_far.xyz);
            float length_z = dot(ray, parameters.direction_far.xyz);
            if (length_z <= 1e-7)
            {
                valid = false;
                continue;
            }
            for (uint z = 0; z < 2; ++z)
            {
                vec3 point_position = origin + ray * ((sliceDistance(cell.z + z) - depth) / length_z);
                valid = valid && finiteVector(vec4(point_position, 1));
                minimum = min(minimum, point_position);
                maximum = max(maximum, point_position);
            }
        }
    }
    // Enclose the complete convex frustum cell, with a scale-relative rounding guard.
    vec3 guard = 1e-4 * (vec3(1) + max(abs(minimum), abs(maximum)));
    bounds[index] = ClusterBounds(vec4(minimum - guard, 0), vec4(maximum + guard, 0));
    if (!valid)
    {
        atomicOr(status[0].values.y, 2u);
    }
    if (index == 0)
    {
        mat4 projection = inverse(parameters.inverse_view_projection);
        if (!finiteVector(projection[0]) || !finiteVector(projection[1]) || !finiteVector(projection[2]) ||
            !finiteVector(projection[3]))
        {
            atomicOr(status[0].values.y, 2u);
        }
        grid_data[0].cells = parameters.cells;
        grid_data[0].limits = parameters.limits;
        grid_data[0].position_near = parameters.position_near;
        grid_data[0].direction_far = parameters.direction_far;
        grid_data[0].world_to_clip = projection;
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
