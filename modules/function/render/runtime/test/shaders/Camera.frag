#version 450
layout(push_constant) uniform Camera { mat4 view; mat4 projection; } camera;
layout(location = 0) out vec4 output_color;
// Fixture oracle only: complete Camera ABI is real; production mesh shaders use both transforms geometrically.
void main() { output_color = camera.view[0] * camera.projection[0][0]; }
