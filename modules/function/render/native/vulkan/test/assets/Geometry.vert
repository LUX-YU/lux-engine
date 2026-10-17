#version 450
layout(location=0) in vec4 position;
layout(location=0) out vec4 vertex_color;
void main()
{
    gl_Position = vec4(position.xyz, 1);
    vertex_color = vec4(position.w);
}
