struct Value
{
    uint value;
};

[[vk::binding(0, 0)]] StructuredBuffer<Value> previous_sample : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<Value> current_sample : register(u1, space0);

[shader("compute")][numthreads(1, 1, 1)] void main()
{
    for (uint i = 0; i < 4; ++i)
    {
        current_sample[i].value = previous_sample[i].value + 1;
    }
}
