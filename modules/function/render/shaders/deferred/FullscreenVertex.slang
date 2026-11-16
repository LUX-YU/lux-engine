#include "../Common.slang"

void luxMain()
{
    // Legacy full-screen triangle: no vertex Buffer, interpolants or View lookup.
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

struct LuxInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
#ifndef LUX_TARGET_DXIL
    uint baseInstance : SV_StartInstanceLocation;
#endif
};

struct LuxOutput
{
    float4 position : SV_Position;
};
[shader("vertex")] LuxOutput main(LuxInput input)
{
    gl_VertexIndex = input.vertexID;
    gl_InstanceIndex = input.instanceID;
#ifndef LUX_TARGET_DXIL
    gl_InstanceIndex += input.baseInstance;
#endif
    luxMain();
    LuxOutput output;
    output.position = gl_Position;
#ifdef LUX_TARGET_DXIL
    output.position.y = -output.position.y;
#endif
    return output;
}
