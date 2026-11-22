[shader("vertex")]
float4 Fullscreen(uint index : SV_VertexID) : SV_Position
{
    float2 uv = float2((index << 1) & 2, index & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 Red() : SV_Target0
{
    return float4(1, 0, 0, 1);
}

[shader("fragment")]
float4 Green(float4 position : SV_Position) : SV_Target0
{
    if (position.x < 32.0) discard;
    return float4(0, 1, 0, 1);
}
