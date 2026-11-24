struct ParticleGpu
{
    float4 position;
    float4 color;
};

[[vk::binding(0, 0)]] StructuredBuffer<ParticleGpu> particles : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<ParticleGpu> trail : register(t1);
[[vk::binding(2, 0)]][format("rgba8")] RWTexture2D<float4> generated : register(u2);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    ParticleGpu particle = id.x < width / 2 ? particles[0] : trail[1];
    generated[id.xy] = particle.color * particle.position.w;
}
