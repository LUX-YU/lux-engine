#version 450

// Test-only Graph-family GBuffer fragment. Shading-model id 0 is the renderer
// client's canonical Unlit tag, so DeferredLighting must copy albedo through
// without light/driver-dependent variation.
layout(location = 1) in vec3 vWorldNormal;
layout(location = 10) in flat vec4 vInstanceTint;

layout(location = 0) out vec4 gAlbedoMetallic;
layout(location = 1) out vec4 gNormalRoughness;
layout(location = 2) out vec4 gEmissiveAo;

vec2 signNotZero(vec2 value)
{
    return vec2(
        value.x >= 0.0 ? 1.0 : -1.0,
        value.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octEncode(vec3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    vec2 packed = normal.xy;
    return normal.z <= 0.0
        ? (1.0 - abs(packed.yx)) * signNotZero(packed)
        : packed;
}

void main()
{
    gAlbedoMetallic = vec4(vInstanceTint.rgb, 0.0);
    gNormalRoughness = vec4(
        octEncode(normalize(vWorldNormal)),
        0.0,
        1.0);
    gEmissiveAo = vec4(0.0, 0.0, 0.0, 1.0);
}
