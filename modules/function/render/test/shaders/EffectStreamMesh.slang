[shader("vertex")]
float4 main([[vk::location(3)]] float3 position : POSITION7) : SV_Position
{
    return float4(position, 1);
}
