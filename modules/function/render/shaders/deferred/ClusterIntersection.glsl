// Count and Fill consume this exact predicate and the same immutable frame inputs.
// A finite spot cone lies inside its range sphere; this intentionally conservative bound
// avoids Legacy's unsafe projected center/radius approximation at the near plane.
struct PointLight { vec4 position_range; vec4 radiance; vec4 attenuation; };
struct SpotLight { vec4 position_range; vec4 radiance; vec4 direction_outer; vec4 attenuation_inner; };
struct ClusterBounds { vec4 minimum; vec4 maximum; };
layout(set = 0, binding = 0, std430) readonly buffer Bounds { ClusterBounds values[]; } bounds;
layout(set = 1, binding = 0, std430) readonly buffer PointLights { PointLight values[]; } point_lights;
layout(set = 1, binding = 1, std430) readonly buffer SpotLights { SpotLight values[]; } spot_lights;
layout(set = 1, binding = 2, std430) readonly buffer LightCounts { uvec4 values; } light_counts;

bool overlapsCluster(ClusterBounds cell, vec4 sphere)
{
    vec3 delta = max(max(cell.minimum.xyz - sphere.xyz, sphere.xyz - cell.maximum.xyz), vec3(0));
    return dot(delta, delta) <= sphere.w * sphere.w;
}

uint saturatedAdd(uint a, uint b)
{
    return a > 0xffffffffu - b ? 0xffffffffu : a + b;
}
