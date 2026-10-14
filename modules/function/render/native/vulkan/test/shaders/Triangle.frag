#version 450
layout(push_constant) uniform Parameters { vec4 color; } parameters;
layout(location = 0) out vec4 output_color;
void main() { output_color = parameters.color; }
