#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0, binding=0) uniform sampler2D textures[];
layout(set=1, binding=0, std430) readonly buffer Values { uvec4 color; } data;
layout(push_constant) uniform Parameters { vec4 control; } parameters;
layout(location=0) out vec4 output_color;
void main()
{
    vec4 sampled = textureLod(textures[nonuniformEXT(uint(parameters.control.x))], gl_FragCoord.xy / vec2(16), 0);
    output_color = sampled * vec4(data.color);
}
