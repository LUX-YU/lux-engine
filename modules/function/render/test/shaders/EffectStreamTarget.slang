[[vk::binding(0, 2)]] Texture2D<float4> source_color[2] : register(t0, space2);

[shader("fragment")]
float4 main() : SV_Target0
{
    return 0.5 * (source_color[0].Load(int3(0, 0, 0)) + source_color[1].Load(int3(0, 0, 0)));
}
