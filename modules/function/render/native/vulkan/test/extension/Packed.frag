#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=2, binding=5) uniform sampler2D textures[];
layout(set=3, binding=7, std430) readonly buffer Values { vec4 color; } data;
layout(set=6, binding=11, std430) readonly buffer Other { vec4 color; } other;
layout(push_constant) uniform Parameters { vec4 control; } parameters;
layout(location=0) out vec4 output_color;
void main()
{
    vec4 sampled = textureLod(textures[nonuniformEXT(uint(parameters.control.x))], gl_FragCoord.xy / vec2(16), 0);
    output_color = sampled * data.color * other.color;
}
