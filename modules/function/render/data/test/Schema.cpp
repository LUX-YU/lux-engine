#include "Golden.data.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #x);                                        \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
using namespace lux::render;

struct Unknown
{
};

static_assert(!DescribedData<Unknown>);
static_assert(DescribedData<CpuOnly> && !GpuData<CpuOnly>);
static_assert(GpuData<ParticleGpu> && !CpuAuthoredGpuData<ParticleGpu>);
static_assert(CpuAuthoredGpuData<LightGpu>);
static_assert(!std::is_trivially_copyable_v<CpuOnly>);

int main()
{
    for (const auto& schema :
         {DataSchema<LightGpu>::descriptor(),
          DataSchema<SpriteGpu>::descriptor(),
          DataSchema<CpuOnly>::descriptor(),
          DataSchema<WireOnly>::descriptor(),
          DataSchema<ComplexGpu>::descriptor(),
          DataSchema<ParticleGpu>::descriptor()})
    {
        CHECK(validateDataSchema(schema));
        CHECK(compatibleDataSchemas(schema, schema));
    }
    auto cpu = DataSchema<CpuOnly>::descriptor();
    CHECK(cpu.cpu && !cpu.gpu && !cpu.wire);
    auto wire = DataSchema<WireOnly>::descriptor();
    CHECK(!wire.cpu && !wire.gpu && wire.wire && wire.wire->version == 3);
    auto wrong = cpu;
    ++wrong.schema_version;
    CHECK(!compatibleDataSchemas(cpu, wrong));
    wrong = cpu;
    wrong.canonical_name = "collision";
    auto collision = compatibleDataSchemas(cpu, wrong);
    CHECK(!collision && collision.error().type == kIdentityCollision);
    SpriteGpu sprite{{1, 2}, 3, {4, 5, 6}};
    constexpr auto layout = DataSchema<SpriteGpu>::gpuLayout();
    static_assert(layout.size == 64 && layout.alignment == 16);
    std::array<std::byte, layout.size> output{};
    CHECK(DataSchema<SpriteGpu>::pack(sprite, output));
    CHECK(!DataSchema<SpriteGpu>::pack(sprite, std::span(output).first(1)));
    for (auto [offset, expected] : {std::pair{0u, 1.f}, {4u, 2.f}, {8u, 3.f}, {16u, 4.f}, {32u, 5.f}, {48u, 6.f}})
    {
        float actual{};
        std::memcpy(&actual, output.data() + offset, 4);
        CHECK(actual == expected);
    }
    constexpr auto complex = DataSchema<ComplexGpu>::gpuLayout();
    static_assert(complex.size == 112);
    CHECK(complex.fields[4].matrix_stride == 16);
    LightGpu light{};
    CHECK(!DataSchema<LightGpu>::pack(light, std::as_writable_bytes(std::span{&light, 1})));
    auto schema = DataSchema<ComplexGpu>::descriptor();
    std::vector<lux::rdesc::DataGpuField> fields(complex.fields.begin(), complex.fields.end());
    schema.gpu->fields = fields;
    fields[4].matrix_stride = 12;
    CHECK(!validateDataSchema(schema));
    fields[4] = complex.fields[4];
    fields[0].array_stride = 1;
    CHECK(!validateDataSchema(schema));
    fields[0] = complex.fields[0];
    fields[0].offset = complex.size;
    CHECK(!validateDataSchema(schema));
    fields[0] = complex.fields[0];
    fields[0].scalar = lux::rdesc::EDataScalar::UINT;
    CHECK(validateDataSchema(schema));
    CHECK(!compatibleDataSchemas(DataSchema<ComplexGpu>::descriptor(), schema));
    // An unchanged fingerprint cannot hide a different complete field contract.
    CpuOnly value{"test", {1, 2}};
    value.*std::get<0>(DataSchema<CpuOnly>::members) = "changed";
    CHECK(value.name == "changed");
    std::puts("Generated DataSchema identities, optional representations and GPU packing PASS");
}
