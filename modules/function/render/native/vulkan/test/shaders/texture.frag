#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(push_constant) uniform Parameters { vec4 control; } parameters;
layout(location = 0) out vec4 output_color;
void main()
{
    uint slot = uint(parameters.control.x);
    float lod = parameters.control.z;
    vec2 size = vec2(textureSize(textures[nonuniformEXT(slot)], int(lod)));
    vec4 value = textureLod(textures[nonuniformEXT(slot)], gl_FragCoord.xy / size, lod);
    // SRGB EOTF is performed exactly once by the SRGB image format. LINEAR is legacy BT.709/D65.
    // DATA bypasses the color transform. Alpha (including linear premultiplication) remains unchanged.
    if (uint(parameters.control.y) != 3u)
    {
        value.rgb = mat3(
            0.6274038959, 0.0690972894, 0.0163914389,
            0.3292830384, 0.9195403951, 0.0880133079,
            0.0433130657, 0.0113623156, 0.8955952532
        ) * value.rgb;
    }
    output_color = value;
}
