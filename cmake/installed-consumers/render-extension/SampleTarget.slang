[[vk::binding(0, 0)]] Texture2D<float4> source_color : register(t0, space0);

[shader("fragment")]
float4 SampleTarget(float4 position : SV_Position) : SV_Target0
{
    return source_color.Load(int3(int2(position.xy), 0));
}
