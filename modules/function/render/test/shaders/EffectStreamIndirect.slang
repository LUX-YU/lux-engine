[[vk::binding(0, 0)]] RWStructuredBuffer<uint> arguments : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> count : register(u1, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
    const uint records[21] = {
        0,
        // One triangle, then a full quad using the same vertex-ID origin.
        3, 1, 0, 0,
        6, 1, 0, 0,
        0,
        6, 1, 0, 0, 0, 0,
        6, 1, 6, 0, 0
    };
    for (uint i = 0; i < 21; ++i)
        arguments[i] = records[i];

    uint argument_count, argument_stride;
    arguments.GetDimensions(argument_count, argument_stride);
    if (argument_count >= 42)
    {
        for (uint i = 0; i < 5; ++i)
            arguments[30 + i] = records[10 + i];
    }

    count[0] = 0;
    count[1] = 0;
    count[2] = 1;
    count[3] = 0;
}
