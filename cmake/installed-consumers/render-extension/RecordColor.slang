struct ColorRecord
{
    float4 color;
};

struct ColorIndex
{
    uint page;
    uint index;
};

[[vk::binding(0, 2)]] StructuredBuffer<ColorRecord> colorPages[] : register(t0, space2);
[[vk::push_constant]] ConstantBuffer<ColorIndex> selection : register(b0, space255);

[shader("fragment")]
float4 RecordColor() : SV_Target0
{
    return colorPages[NonUniformResourceIndex(selection.page)][selection.index].color;
}
