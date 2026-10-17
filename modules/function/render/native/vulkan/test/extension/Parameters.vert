#version 450
layout(set=2, binding=9, std430) readonly buffer Values { vec4 color; } data;
layout(push_constant) uniform Parameters { vec4 control; vec4 view; vec4 gain; vec4 bias; } parameters;
void main()
{
    const vec2 positions[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(positions[gl_VertexIndex], 0, 1);
    gl_Position.x += parameters.gain.x - 0.5 + data.color.x - 0.5;
}
