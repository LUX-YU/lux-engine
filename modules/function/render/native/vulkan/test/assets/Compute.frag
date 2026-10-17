#version 450
layout(set=2, binding=3, std430) readonly buffer Values { vec4 color; } data;
layout(location=0) out vec4 output_color;
void main()
{
    output_color = data.color;
}
