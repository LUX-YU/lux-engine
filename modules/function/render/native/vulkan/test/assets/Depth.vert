#version 450
layout(push_constant) uniform Parameters { vec4 color; vec4 controls; } parameters;
void main()
{
    const vec2 points[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(points[gl_VertexIndex], parameters.controls.x, 1);
}
