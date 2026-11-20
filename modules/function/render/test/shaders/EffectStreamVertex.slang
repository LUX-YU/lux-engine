[shader("vertex")]
float4 main(uint id : SV_VertexID) : SV_Position
{
    const float2 corners[6] = {
        float2(0, -1), float2(1, -1), float2(1, 1),
        float2(0, -1), float2(1, 1), float2(0, 1)
    };
    const float2 position = corners[id % 6] + float2(id < 6 ? -1 : 0, 0);
    return float4(position, 0.25, 1);
}
