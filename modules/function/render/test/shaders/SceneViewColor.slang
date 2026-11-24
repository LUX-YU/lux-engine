#include "../../shaders/SceneView.slang"

[[vk::binding(0, 0)]] StructuredBuffer<SceneViewGpu> scene_view : register(t0, space0);
[[vk::binding(1, 0)]][format("rgba8")] RWTexture2D<float4> generated : register(u1, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (all(id.xy < uint2(width, height))) generated[id.xy] = scene_view[0].background;
}
