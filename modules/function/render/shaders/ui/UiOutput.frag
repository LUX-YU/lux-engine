#version 460
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform texture2D linear_color;
layout(location = 0) out vec4 result;
void main()
{
    result = texelFetch(linear_color, ivec2(gl_FragCoord.xy), 0);
#ifdef OUTPUT_SRGB
    vec3 color = max(result.rgb, vec3(0));
    result.rgb = mix(1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, 12.92 * color,
        lessThanEqual(color, vec3(0.0031308)));
#endif
}
