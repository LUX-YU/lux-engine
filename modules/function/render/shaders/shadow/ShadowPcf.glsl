// Neutral view/slice ports. No fixed View domain or sibling Feature state.
struct VisibilityView { mat4 world_to_clip; vec4 planes[6]; uvec4 flags; };
struct ShadowSlice { vec4 uv_scale_bias; vec4 inner_uv; vec4 depths; vec4 sampling; uvec4 identity; };
layout(set = 2, binding = 0, std430) readonly buffer ShadowViews { VisibilityView records[]; } shadow_views;
layout(set = 2, binding = 1, std430) readonly buffer ShadowSlices { ShadowSlice records[]; } shadow_slices;
layout(set = 2, binding = 2, std430) readonly buffer ShadowRanges { uvec4 words[]; } shadow_ranges;
layout(set = 2, binding = 3) uniform texture2DArray shadow_depth;
layout(set = 2, binding = 4) uniform samplerShadow shadow_compare;

uint pointShadowFace(vec3 delta)
{
    vec3 magnitude = abs(delta);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) return delta.x >= 0 ? 0 : 1;
    if (magnitude.y >= magnitude.z) return delta.y >= 0 ? 2 : 3;
    return delta.z >= 0 ? 4 : 5;
}

float shadowVisibility(uint kind, uint light_index, vec3 position, vec3 normal, vec3 light_position)
{
    uvec4 mapping = shadow_ranges.words[3 + shadow_ranges.words[0][kind] + light_index];
    if (mapping.y == 0) return 1;
    uint index = mapping.x;
    if (kind == 0)
    {
        vec3 camera = uintBitsToFloat(shadow_ranges.words[1]).xyz;
        vec3 direction = uintBitsToFloat(shadow_ranges.words[2]).xyz;
        float distance = dot(position - camera, direction);
        bool found = false;
        // Legacy selectCascade, with explicit absolute distances and no implicit last-cascade fallback.
        for (uint cascade = 0; cascade < mapping.y; ++cascade)
        {
            vec2 interval = shadow_slices.records[mapping.x + cascade].depths.xy;
            if (distance >= interval.x && distance <= interval.y)
            {
                index = mapping.x + cascade;
                found = true;
                break;
            }
        }
        if (!found) return 1;
    }
    else if (kind == 1)
    {
        index += pointShadowFace(position - light_position);
    }
    ShadowSlice slice = shadow_slices.records[index];
    if (slice.identity.y == 0) return 1;
    vec4 clip = shadow_views.records[index].world_to_clip * vec4(position + normal * slice.sampling.x, 1);
    if (clip.w <= 0) return 1;
    vec3 projected = clip.xyz / clip.w;
    if (projected.z < 0 || projected.z > 1 || any(greaterThan(abs(projected.xy), vec2(1)))) return 1;
    vec2 uv = (projected.xy * 0.5 + 0.5) * slice.uv_scale_bias.xy + slice.uv_scale_bias.zw;
    vec2 texel = 1.0 / vec2(textureSize(shadow_depth, 0).xy);
    int radius = int(slice.sampling.z);
    float result = 0;
    // Legacy atlas-safe PCF footprint. Bias domains remain separate and there are no derivatives
    // inside divergent light/cascade branches; receiver-plane heuristics are deliberately not copied.
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            vec2 sample_uv = clamp(uv + vec2(x, y) * texel, slice.inner_uv.xy, slice.inner_uv.zw);
            result += texture(sampler2DArrayShadow(shadow_depth, shadow_compare),
                              vec4(sample_uv, float(slice.identity.x), projected.z - slice.sampling.y));
        }
    }
    return result / float((radius * 2 + 1) * (radius * 2 + 1));
}
