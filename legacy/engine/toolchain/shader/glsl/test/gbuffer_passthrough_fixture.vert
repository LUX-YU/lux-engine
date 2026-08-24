#version 450

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 3) out vec2 vUV;
layout(location = 5) out vec3 vWorldTangent;
layout(location = 7) flat out float vTransitionCoverage;
layout(location = 8) flat out uint vTransitionSeed;
layout(location = 9) flat out uint vTransitionFadeOut;

void main()
{
    vWorldPos = vec3(0.0);
    vWorldNormal = vec3(0.0, 0.0, 1.0);
    vUV = vec2(0.0);
    vWorldTangent = vec3(1.0, 0.0, 0.0);
    vTransitionCoverage = 1.0;
    vTransitionSeed = 0u;
    vTransitionFadeOut = 0u;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
