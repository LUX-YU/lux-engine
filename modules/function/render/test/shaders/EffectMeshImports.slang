struct Property
{
    uint mesh_index;
    uint material_buffer;
    uint material_record;
    uint section_index;
    uint render_flags;
    uint transform_record;
};

struct Transform
{
    float4 rows[4];
};

struct Word
{
    uint value;
};

#ifdef PAGED_INSTANCES
struct Address
{
    uint2 value;
};

struct PageAddresses
{
    uint2 properties;
    uint2 alive;
};

uint64_t address(uint2 words)
{
    return uint64_t(words.x) | (uint64_t(words.y) << 32);
}

[[vk::binding(0, 0)]] StructuredBuffer<Address> transforms : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<Address> properties : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<Address> alive : register(t2, space0);
#else
[[vk::binding(0, 0)]] StructuredBuffer<Transform> transforms : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<Property> properties : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<Word> alive : register(t2, space0);
#endif

[[vk::binding(3, 0)]] StructuredBuffer<Word> selections : register(t3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<Word> result : register(u4, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
    const uint count = selections[0].value;
    uint flags = 0;
    uint present = 0;
    float value = 0;
    uint record = 0;
    if (count != 0)
    {
#ifdef PAGED_INSTANCES
        const PageAddresses property_page = ((PageAddresses*)address(properties[0].value))[0];
        const PageAddresses alive_page = ((PageAddresses*)address(alive[0].value))[0];
        const Property item = ((Property*)address(property_page.properties))[0];
        present = ((Word*)address(alive_page.alive))[0].value;
        record = item.transform_record;
        value = ((Transform*)address(transforms[record >> 14].value))[record & 16383].rows[0].x;
#else
        const Property item = properties[0];
        present = alive[0].value;
        record = item.transform_record;
        value = transforms[record].rows[0].x;
#endif
        flags = item.render_flags;
    }
    result[0].value = count;
    result[1].value = flags;
    result[2].value = present;
    result[3].value = asuint(value);
    result[4].value = record;
}
