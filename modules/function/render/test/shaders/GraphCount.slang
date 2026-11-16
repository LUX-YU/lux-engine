struct Value
{
    uint value;
};

struct Parameters
{
    uint requested;
};

[[vk::binding(0, 3)]] RWStructuredBuffer<Value> commands : register(u0, space3);
[[vk::binding(1, 3)]] RWStructuredBuffer<Value> count : register(u1, space3);
[[vk::binding(2, 3)]] RWStructuredBuffer<Value> indices : register(u2, space3);
[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("compute")][numthreads(1, 1, 1)] void main()
{
    for (uint i = 0; i < 3; ++i)
    {
        uint offset = i * 5;
        commands[offset].value = 3;
        commands[offset + 1].value = 1;
        commands[offset + 2].value = 0;
        commands[offset + 3].value = 0;
        commands[offset + 4].value = 0;
        indices[i].value = i;
    }
    count[0].value = parameters.requested;
}
