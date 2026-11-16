[shader("vertex")] float4 main(uint vertex : SV_VertexID) : SV_Position
{
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    float4 position = float4(positions[vertex], 0.25, 1);
#if LUX_TARGET_DXIL
    position.y = -position.y;
#endif
    return position;
}
