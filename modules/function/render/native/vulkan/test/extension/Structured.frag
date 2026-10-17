#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=4, binding=8) uniform sampler2D textures[];
layout(set=2, binding=9, std430) readonly buffer Values { mat4 matrix; vec4 colors[2]; } data;
layout(push_constant) uniform Parameters { vec4 control; } parameters;
layout(location=0) out vec4 output_color;
void main()
{
    vec4 sampled = textureLod(textures[nonuniformEXT(uint(parameters.control.x))], gl_FragCoord.xy / vec2(16), 0);
    output_color = sampled * (data.matrix * data.colors[1]);
}
