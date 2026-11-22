[[vk::binding(7, 4)]][format("rgba8")] RWTexture2D<float4> generated : register(u7, space4);

struct OutputConstants
{
    float red;
};

[[vk::push_constant]] ConstantBuffer<OutputConstants> parameters : register(b0, space255);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (all(id.xy < uint2(width, height)))
        generated[id.xy] = float4(parameters.red, 0.5, 0.75, 1.0);
}
