struct ClusterGrid
{
    uvec4 cells;
    uvec4 limits;
    vec4 position_near;
    vec4 direction_far;
    mat4 world_to_clip;
};

[[vk::binding(0, 3)]] StructuredBuffer<ClusterGrid> cluster_grid : register(t0, space3);

struct Lux_cluster_offsets
{
    uint value;
};

[[vk::binding(1, 3)]] StructuredBuffer<Lux_cluster_offsets> cluster_offsets : register(t1, space3);

struct Lux_cluster_indices
{
    uint value;
};

[[vk::binding(2, 3)]] StructuredBuffer<Lux_cluster_indices> cluster_indices : register(t2, space3);

struct ClusterStatus
{
    uvec4 values;
};

[[vk::binding(3, 3)]] StructuredBuffer<ClusterStatus> cluster_status : register(t3, space3);

bool clusterRange(vec3 position, out uint first, out uint end)
{
    if (cluster_status[0].values.y != 0)
    {
        return false;
    }
    vec4 clip = mul(cluster_grid[0].world_to_clip, vec4(position, 1));
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    float depth = dot(position - cluster_grid[0].position_near.xyz, cluster_grid[0].direction_far.xyz);
    float near_z = cluster_grid[0].position_near.w, far_z = cluster_grid[0].direction_far.w;
    // Coverage outside this grid uses the complete list rather than silently losing lights.
    if (clip.w <= 0 || any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1))) || depth < near_z || depth > far_z)
    {
        return false;
    }
    float z =
        cluster_grid[0].cells.w == 0 ? (depth - near_z) / (far_z - near_z) : log(depth / near_z) / log(far_z / near_z);
    uvec3 cell = min(uvec3(vec3(uv, z) * vec3(cluster_grid[0].cells.xyz)), cluster_grid[0].cells.xyz - 1);
    uint index = cell.x + cluster_grid[0].cells.x * (cell.y + cluster_grid[0].cells.y * cell.z);
    first = cluster_offsets[index].value;
    end = cluster_offsets[index + 1].value;
    return true;
}
