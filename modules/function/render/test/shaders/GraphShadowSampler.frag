#version 450
layout(set = 0, binding = 0) uniform texture2DArray depths;
layout(set = 0, binding = 1) uniform sampler ordinary;
layout(set = 0, binding = 2) uniform samplerShadow comparison;
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(texture(sampler2DArrayShadow(depths, comparison), vec4(.5, .5, 0, .5)),
                 texture(sampler2DArrayShadow(depths, comparison), vec4(.5, .5, 1, .5)),
                 texture(sampler2DArray(depths, ordinary), vec3(.5, .5, 0)).r, 1);
}
