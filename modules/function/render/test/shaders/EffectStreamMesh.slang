[shader("vertex")]
float4 main([[vk::location(0)]] float3 position : TEXCOORD0) : SV_Position
{
    return float4(position, 1);
}
