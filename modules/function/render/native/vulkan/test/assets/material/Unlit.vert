#version 450
layout(location=3) out vec2 vUV;
layout(location=4) out flat uint vMatIndex;
layout(location=7) out flat float vTransitionCoverage;
layout(location=8) out flat uint vTransitionSeed;
layout(location=9) out flat uint vTransitionFadeOut;
void main()
{
    const vec2 points[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(points[gl_VertexIndex],0,1);
    vUV=vec2(.5);
    vMatIndex=0; vTransitionCoverage=1; vTransitionSeed=0; vTransitionFadeOut=0;
}

