#version 450
layout(push_constant) uniform Parameters { vec4 color; vec4 controls; } parameters;
layout(location=0) out vec4 color;
void main() { color = parameters.color; }
