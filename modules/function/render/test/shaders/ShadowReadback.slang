[[vk::binding(0, 0)]] StructuredBuffer<uint> shadow_views : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<uint> shadow_commands : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> views : register(u2, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> commands : register(u3, space0);

[shader("compute")][numthreads(64, 1, 1)]
void main(uint3 dispatch : SV_DispatchThreadID)
{
    uint index = dispatch.x;
    uint input_count, output_count, stride;
    shadow_views.GetDimensions(input_count, stride);
    // The caster, receiver and marker have two five-word indexed commands per view.
    uint draw_words = 3 * (input_count / 44) * 10;
    views.GetDimensions(output_count, stride);
    if (index < output_count)
        views[index] = index < input_count ? shadow_views[index] : 0;
    shadow_commands.GetDimensions(input_count, stride);
    commands.GetDimensions(output_count, stride);
    if (index < output_count)
        commands[index] = index < min(draw_words, input_count) ? shadow_commands[index] : 0;
}
