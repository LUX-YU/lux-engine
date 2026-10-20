#version 460
layout(set = 2, binding = 3) uniform texture2D source_image;
layout(set = 2, binding = 4) uniform sampler source_sampler;
layout(std140, set = 3, binding = 5) uniform Tint { vec4 value; } tint;
layout(location = 0) out vec4 first_color;
layout(location = 1) out vec4 second_color;
void main()
{
    first_color = texture(sampler2D(source_image, source_sampler), vec2(0.5)) * tint.value;
    second_color = vec4(0, 0.25, 0.5, 1);
}
