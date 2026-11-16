#include "../Common.slang"

// Legacy downsample.comp: forward-depth maximum, one dispatch per mip.
// The normalized source footprint includes the odd-size edge omitted by a fixed 2x2 reduction.

[[vk::binding(0, 0)]] Texture2D<float4> uSrc : register(t0, space0);
[[vk::image_format("r32f")]] [[vk::binding(1, 0)]] RWTexture2D<float> uDst : register(u1, space0);

void luxMain()
{
    uvec2 destination = gl_GlobalInvocationID.xy;
    uvec2 destination_size = uvec2(imageSize(uDst));
    if (any(greaterThanEqual(destination, destination_size)))
    {
        return;
    }
    uvec2 source_size = uvec2(textureSize(uSrc, 0));
    uvec2 first = destination * source_size / destination_size;
    uvec2 end = ((destination + 1u) * source_size + destination_size - 1u) / destination_size;
    float farthest = 0.0;
    for (uint y = first.y; y < end.y; ++y)
    {
        for (uint x = first.x; x < end.x; ++x)
        {
            farthest = max(farthest, texelFetch(uSrc, ivec2(x, y), 0).r);
        }
    }
    uDst[int2(destination)] = farthest;
}

[shader("compute")][numthreads(8, 8, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
