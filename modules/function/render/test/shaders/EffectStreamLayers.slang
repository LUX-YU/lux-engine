[[vk::binding(0, 0)]] Texture2DArray<float4> layers : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> generated : register(u1, space0);
[[vk::binding(2, 0)]] Texture2DArray<float> depth_layers : register(t2, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height, count;
    layers.GetDimensions(width, height, count);
    uint value = 0;
    if (id.x == 0)
    {
        value = count;
    }
    else if (id.x <= count && id.x <= 3)
    {
        value = uint(round(layers.Load(int4(16, 16, id.x - 1, 0)).r * 255.0));
    }
    else if (id.x >= 4 && id.x <= count + 3 && id.x <= 6)
    {
        value = asuint(depth_layers.Load(int4(16, 16, id.x - 4, 0)));
    }
    generated[id.x] = value;
}
