[shader("vertex")]
float4 Fullscreen(float2 position : POSITION) : SV_Position
{
    return float4(position, 0.0, 1.0);
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
