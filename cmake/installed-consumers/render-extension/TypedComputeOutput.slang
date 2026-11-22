#include "ComputeOutput.slang"

struct OutputChannels
{
    float4 channels;
};

[[vk::binding(1, 0)]] ConstantBuffer<OutputChannels> colors : register(b1, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void typed(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (all(id.xy < uint2(width, height)))
        generated[id.xy] = float4(parameters.red, colors.channels.yzw);
}
