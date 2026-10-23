#version 460
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D source_images[];
layout(set = 0, binding = 1) uniform sampler source_sampler;
layout(push_constant) uniform Parameters { vec2 scale; vec2 translation; uint source_index; } parameters;
layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 result;
void main()
{
    result = color * texture(sampler2D(source_images[parameters.source_index], source_sampler), texture_uv);
}
