#version 450
layout(location=0) in vec3 position;
#ifndef DEPTH_ONLY
layout(location=1) in vec2 uv;
layout(location=3) out vec2 vUV;
layout(location=4) out flat uint vMatIndex;
layout(location=7) out flat float vTransitionCoverage;
layout(location=8) out flat uint vTransitionSeed;
layout(location=9) out flat uint vTransitionFadeOut;
#endif
struct Transform { mat4 model; };
struct Word { uint value; };
layout(set=3,binding=7,std430) readonly buffer Transforms { Transform records[]; } transforms;
layout(set=2,binding=6,std430) readonly buffer Members { Word words[]; } members;
layout(push_constant) uniform Camera { mat4 view; mat4 projection; } camera;
void main()
{
    uint slot = members.words[5 + gl_InstanceIndex].value;
    gl_Position = camera.projection * camera.view * transforms.records[slot].model * vec4(position, 1);
#ifndef DEPTH_ONLY
    vUV = uv;
    vMatIndex = 0;
    vTransitionCoverage = 1;
    vTransitionSeed = 0;
    vTransitionFadeOut = 0;
#endif
}
