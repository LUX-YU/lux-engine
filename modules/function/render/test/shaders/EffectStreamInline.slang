struct Parameters
{
    uint value;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("fragment")]
float4 main() : SV_Target0
{
    return float4(parameters.value & 255u, 0, 0, 255) / 255.0;
}
