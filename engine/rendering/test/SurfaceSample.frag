#version 460
layout(set = 0, binding = 0) uniform texture2D source_image;
layout(set = 0, binding = 1) uniform sampler source_sampler;
layout(location = 0) out vec4 color;
void main()
{
    color = texture(sampler2D(source_image, source_sampler), vec2(0.5));
}
