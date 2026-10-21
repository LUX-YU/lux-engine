#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D hdr_color;
layout(push_constant) uniform Parameters { float exposure; } parameters;
layout(location = 0) out vec4 color;

// Legacy tonemap.frag.lglsl::toneMapACES, preserving its actual coefficients.
vec3 toneMapACES(vec3 value)
{
    return clamp((value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec3 value = max(texelFetch(hdr_color, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0)) * exp2(parameters.exposure);
    value = toneMapACES(value);
#ifdef OUTPUT_SRGB
    value = mix(1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, 12.92 * value, lessThanEqual(value, vec3(0.0031308)));
#endif
    color = vec4(value, 1.0);
}
