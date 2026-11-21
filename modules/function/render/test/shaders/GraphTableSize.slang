struct Parameters
{
    uint requested;
};

[[vk::binding(0, 0)]] Texture2D<float4> images[] : register(t0, space0);
[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("fragment")] float4 main() : SV_Target0
{
    uint width, height;
    images[NonUniformResourceIndex(parameters.requested)].GetDimensions(width, height);
    return float4(width / 255.0, height / 255.0, 0, 1);
}
