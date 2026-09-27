#version 450
layout(push_constant) uniform Params { uint scene_index; uint view_index; float red; float green; float blue; float alpha; } params;
layout(location = 0) out vec4 result;
void main() { result = vec4(params.red, params.green, params.blue, params.alpha); }
