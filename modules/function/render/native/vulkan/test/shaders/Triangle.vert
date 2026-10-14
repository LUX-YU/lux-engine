#version 450
const vec2 vertices[3] = vec2[3](vec2(-0.75, -0.75), vec2(0.75, -0.75), vec2(0.0, 0.75));
void main() { gl_Position = vec4(vertices[gl_VertexIndex], 0.0, 1.0); }
