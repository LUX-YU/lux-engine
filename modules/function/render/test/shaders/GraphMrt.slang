[[vk::binding(3, 2)]] Texture2D<float4> source_image : register(t3, space2);
[[vk::binding(4, 2)]] SamplerState source_sampler : register(s4, space2);

struct Tint
{
    float4 value;
};

[[vk::binding(5, 3)]] ConstantBuffer<Tint> tint : register(b5, space3);

struct Output
{
    float4 first_color : SV_Target0;
    float4 second_color : SV_Target1;
};

[shader("fragment")] Output main()
{
    Output result;
    result.first_color = source_image.Sample(source_sampler, float2(0.5)) * tint.value;
    result.second_color = float4(0, 0.25, 0.5, 1);
    return result;
}
