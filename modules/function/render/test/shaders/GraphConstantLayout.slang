struct Entry
{
    uint4 groups[2];
};

struct UniformData
{
    Entry entries[2];
};

struct Parameters
{
    column_major float4x2 basis;
    uint4 ids[2];
};

struct Readback
{
    uint4 values;
};

[[vk::binding(0, 0)]] ConstantBuffer<UniformData> uniforms : register(b0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<Readback> output_values : register(u1, space0);
[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("compute")][numthreads(1, 1, 1)] void main()
{
    output_values[0].values = uint4(
        asuint(parameters.basis[2][1]),
        parameters.ids[1].z,
        uniforms.entries[1].groups[1].y,
        0
    );
}
