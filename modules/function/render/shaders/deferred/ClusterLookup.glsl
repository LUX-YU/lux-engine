layout(set = 3, binding = 0, std430) readonly buffer ClusterGrid
{
    uvec4 cells; uvec4 limits;
    vec4 position_near; vec4 direction_far;
    mat4 world_to_clip;
} cluster_grid;
layout(set = 3, binding = 1, std430) readonly buffer ClusterOffsets { uint values[]; } cluster_offsets;
layout(set = 3, binding = 2, std430) readonly buffer ClusterIndices { uint values[]; } cluster_indices;
layout(set = 3, binding = 3, std430) readonly buffer ClusterStatus { uvec4 values; } cluster_status;
bool clusterRange(vec3 position, out uint first, out uint end)
{
    if (cluster_status.values.y != 0) return false;
    vec4 clip = cluster_grid.world_to_clip * vec4(position, 1);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    float depth = dot(position - cluster_grid.position_near.xyz, cluster_grid.direction_far.xyz);
    float near_z = cluster_grid.position_near.w, far_z = cluster_grid.direction_far.w;
    // Coverage outside this grid uses the complete list rather than silently losing lights.
    if (clip.w <= 0 || any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1))) || depth < near_z || depth > far_z)
        return false;
    float z = cluster_grid.cells.w == 0 ? (depth - near_z) / (far_z - near_z) : log(depth / near_z) / log(far_z / near_z);
    uvec3 cell = min(uvec3(vec3(uv, z) * vec3(cluster_grid.cells.xyz)), cluster_grid.cells.xyz - 1);
    uint index = cell.x + cluster_grid.cells.x * (cell.y + cluster_grid.cells.y * cell.z);
    first = cluster_offsets.values[index]; end = cluster_offsets.values[index + 1];
    return true;
}
