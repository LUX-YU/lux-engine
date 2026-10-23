#version 460
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 vertex_color;
layout(push_constant) uniform Parameters { vec2 scale; vec2 translation; uint source_index; } parameters;
layout(location = 0) out vec2 texture_uv;
layout(location = 1) out vec4 color;
void main()
{
    gl_Position = vec4(position * parameters.scale + parameters.translation, 0, 1);
    texture_uv = uv;
    vec3 encoded = vertex_color.rgb;
    color = vec4(mix(pow((encoded + 0.055) / 1.055, vec3(2.4)), encoded / 12.92,
        lessThanEqual(encoded, vec3(0.04045))), vertex_color.a);
}
