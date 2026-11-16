// Count and Fill consume this exact predicate and the same immutable frame inputs.
// A finite spot cone lies inside its range sphere; this intentionally conservative bound
// avoids Legacy's unsafe projected center/radius approximation at the near plane.
struct PointLight
{
    vec4 position_range;
    vec4 radiance;
    vec4 attenuation;
};

struct SpotLight
{
    vec4 position_range;
    vec4 radiance;
    vec4 direction_outer;
    vec4 attenuation_inner;
};

struct ClusterBounds
{
    vec4 minimum;
    vec4 maximum;
};

[[vk::binding(0, 0)]] StructuredBuffer<ClusterBounds> bounds : register(t0, space0);
[[vk::binding(0, 1)]] StructuredBuffer<PointLight> point_lights : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<SpotLight> spot_lights : register(t1, space1);

struct LightCounts
{
    uvec4 values;
};

[[vk::binding(2, 1)]] StructuredBuffer<LightCounts> light_counts : register(t2, space1);

bool overlapsCluster(ClusterBounds cell, vec4 sphere)
{
    vec3 delta = max(max(cell.minimum.xyz - sphere.xyz, sphere.xyz - cell.maximum.xyz), vec3(0));
    return dot(delta, delta) <= sphere.w * sphere.w;
}

uint saturatedAdd(uint a, uint b)
{
    return a > 0xffffffffu - b ? 0xffffffffu : a + b;
}
