#version 460
layout(set = 5, binding = 0) uniform texture2D colors[2];
layout(set = 5, binding = 2) uniform texture2D depth_image;
layout(set = 5, binding = 3) uniform sampler source_sampler;
layout(location = 0) out vec4 result;
void main()
{
    float red = texture(sampler2D(colors[0], source_sampler), vec2(0.5)).r;
    float green = texture(sampler2D(colors[1], source_sampler), vec2(0.5)).g;
    float depth = texture(sampler2D(depth_image, source_sampler), vec2(0.5)).r;
    result = vec4(red, green, depth, 1);
}
