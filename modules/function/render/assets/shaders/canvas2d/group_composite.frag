#version 450
// Canvas2D offscreen-group composite (A2-04): the group RT holds PREMULTIPLIED
// 2D content over transparent black; blitting it with the canvas blend
// (ONE / ONE_MINUS_SRC_ALPHA) composites the whole group over color_target in
// one fullscreen triangle (tonemap.vert provides vUV).
layout(set = 1, binding = 0) uniform sampler2D uGroup;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(uGroup, vUV);
}
