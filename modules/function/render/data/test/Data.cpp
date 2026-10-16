#include "Fixtures.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>
#include <thread>

using namespace lux::render;
namespace cxx = lux::cxx;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

static constexpr EncodedDataLimits kLimits{4096, 4096};
static_assert(EncodableData<fixture::Record> && EncodableData<fixture::Payload>);
static_assert(!EncodableData<int*> && !EncodableData<std::span<int>> && !EncodableData<std::string_view>);
static_assert(!EncodableData<std::vector<std::byte>> && !EncodableData<std::array<int*, 2>>);
static_assert(noexcept(decodeData<fixture::Record>(std::declval<RetainableEncodedDataView>(), kLimits)));

static lux::cxx::SharedBytes<> owned(std::span<const std::byte> bytes)
{
    return lux::cxx::SharedBytes<>::copyOf(bytes);
}

static std::vector<std::byte> mutableCopy(const lux::cxx::SharedBytes<>& bytes)
{
    return {bytes.view().begin(), bytes.view().end()};
}

static int schemaTests()
{
    const auto& original = DataSchema<fixture::Record>::describe();
    CHECK(validateDataSchema(original));
    CHECK(compatibleDataSchemas(original, original));
    auto changed = original;
    changed.type = DataTypeIdView("different.name");
    CHECK(!compatibleDataSchemas(original, changed));
    CHECK(!DataTypeIdView::fromVerified("different.name", original.type.hash()).isValid());
    DataTypeId name("test.g2.record");
    CHECK(name.view() == original.type);
    changed = original;
    ++changed.schema_version.minor;
    CHECK(!compatibleDataSchemas(original, changed));
    changed = original;
    ++changed.encoding_version.minor;
    CHECK(!compatibleDataSchemas(original, changed));
    changed = original;
    changed.alignment = 2;
    CHECK(validateDataSchema(changed) && !compatibleDataSchemas(original, changed));
    changed.alignment = 3;
    CHECK(!validateDataSchema(changed));
    changed.alignment = 0;
    CHECK(!validateDataSchema(changed));
    changed = original;
    ++changed.fixed_size;
    CHECK(!validateDataSchema(changed));
    changed = original;
    changed.schema_version.major = 0;
    CHECK(!validateDataSchema(changed));
    std::vector<DataField> fields(original.fields.begin(), original.fields.end());
    changed = original;
    changed.fields = fields;
    fields[0].scalar = cxx::EScalarKind::F32;
    CHECK(validateDataSchema(changed) && !compatibleDataSchemas(original, changed));
    fields.assign(original.fields.begin(), original.fields.end());
    std::swap(fields[0], fields[1]);
    CHECK(validateDataSchema(changed) && !compatibleDataSchemas(original, changed));
    fields.assign(original.fields.begin(), original.fields.end());
    fields[1].count = 2;
    changed.fixed_size = 14;
    changed.alignment = 2;
    CHECK(validateDataSchema(changed) && !compatibleDataSchemas(original, changed));
    fields[1].count = 0;
    CHECK(!validateDataSchema(changed));
    fields.assign(original.fields.begin(), original.fields.end());
    changed.fixed_size = 16;
    fields[1].name = fields[0].name;
    CHECK(!validateDataSchema(changed));
    fields.assign(original.fields.begin(), original.fields.end());
    fields[0].scalar = cxx::EScalarKind::BYTES;
    CHECK(!validateDataSchema(changed));
    fields.assign(original.fields.begin(), original.fields.end());
    const std::uint64_t duplicates[]{1, 1};
    fields[4].enum_values = duplicates;
    CHECK(!validateDataSchema(changed));
    const std::uint64_t too_large[]{256};
    fields[4].enum_values = too_large;
    CHECK(!validateDataSchema(changed));
    CHECK(renderDataErrorDescriptors().size() == 6);
    fields.assign(original.fields.begin(), original.fields.end());
    const std::uint64_t different_modes[]{1, 2};
    fields[4].enum_values = different_modes;
    CHECK(validateDataSchema(changed) && !compatibleDataSchemas(original, changed));
    return 0;
}

static int roundtripTests()
{
    fixture::Record input{0x12345678, {-32768, 0, 32767}, 0.25f, true, 3};
    auto bytes = encodeData(input, {}, kLimits);
    CHECK(bytes && bytes->size() == 16);
    const std::byte golden[]{
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
        std::byte{0}, std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{0xff}, std::byte{0x7f},
        std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x3e}, std::byte{1}, std::byte{3}
    };
    CHECK(std::ranges::equal(bytes->view(), golden));
    auto result = decodeData<fixture::Record>({DataSchema<fixture::Record>::describe(), *bytes, {}}, kLimits);
    CHECK(result && *result == input);
    auto unaligned_storage = std::make_shared<std::vector<std::byte>>(17);
    std::copy(bytes->view().begin(), bytes->view().end(), unaligned_storage->begin() + 1);
    auto unaligned = lux::cxx::SharedBytes<>::fromOwner(unaligned_storage, std::span(*unaligned_storage).subspan(1));
    CHECK(decodeData<fixture::Record>({DataSchema<fixture::Record>::describe(), unaligned, {}}, kLimits));
    input.weight = -0.0f;
    bytes = encodeData(input, {}, kLimits);
    CHECK(bytes);
    result = decodeData<fixture::Record>({DataSchema<fixture::Record>::describe(), *bytes, {}}, kLimits);
    CHECK(result && std::bit_cast<std::uint32_t>(result->weight) == 0);
    input.weight = std::numeric_limits<float>::quiet_NaN();
    bytes = encodeData(input, {}, kLimits);
    CHECK(bytes);
    result = decodeData<fixture::Record>({DataSchema<fixture::Record>::describe(), *bytes, {}}, kLimits);
    CHECK(result && std::bit_cast<std::uint32_t>(result->weight) == 0x7fc00000);

    const std::byte source[]{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    std::array attachments{owned(source)};
    fixture::Payload payload{owned(source), {0, 4, 1}, {}};
    auto packet = encodeData(payload, attachments, kLimits);
    CHECK(packet);
    auto decoded = decodeData<fixture::Payload>(
        {DataSchema<fixture::Payload>::describe(), *packet, attachments}, kLimits
    );
    CHECK(decoded && std::ranges::equal(decoded->blob.view(), source));
    CHECK(decoded->external_bytes.size() == 1 && decoded->external_bytes.data()[0] == std::byte{5});
    return 0;
}

static int malformedTests()
{
    auto bytes = encodeData(fixture::Record{}, {}, kLimits);
    CHECK(bytes);
    const auto& schema = DataSchema<fixture::Record>::describe();
    for (std::size_t size = 0; size < bytes->size(); ++size)
    {
        auto truncated = owned(bytes->view().first(size));
        CHECK(!decodeData<fixture::Record>({schema, truncated, {}}, kLimits));
    }
    auto check_bad = [&](std::vector<std::byte> data)
    {
        auto packet = owned(data);
        auto before = fixture::record_decode_calls;
        auto result = decodeData<fixture::Record>({schema, packet, {}}, kLimits);
        return !result && fixture::record_decode_calls == before;
    };
    auto data = mutableCopy(*bytes);
    data.push_back(std::byte{0});
    CHECK(check_bad(data));
    data = mutableCopy(*bytes);
    data[14] = std::byte{2};
    CHECK(check_bad(data));
    data = mutableCopy(*bytes);
    data[15] = std::byte{2};
    CHECK(check_bad(data));
    data = mutableCopy(*bytes);
    data[13] = std::byte{0x80}; // noncanonical negative zero
    CHECK(check_bad(data));
    CHECK(!decodeData<fixture::Record>({schema, *bytes, {}}, {15, 100}));
    CHECK(!encodeData(fixture::Record{}, {}, {15, 100}));
    fixture::Record bad;
    bad.mode = 2;
    CHECK(!encodeData(bad, {}, kLimits));
    auto mismatch = schema;
    ++mismatch.encoding_version.major;
    auto before = fixture::record_decode_calls;
    CHECK(!decodeData<fixture::Record>({mismatch, *bytes, {}}, kLimits));
    CHECK(before == fixture::record_decode_calls);
    auto moved = std::move(*bytes);
    CHECK(!validateEncodedData({schema, *bytes, {}}, kLimits)); // moved-from SharedBytes can retain size
    CHECK(validateEncodedData({schema, moved, {}}, kLimits));
    return 0;
}

static int referenceTests()
{
    const std::array<std::byte, 8> source{};
    auto bytes = owned(source);
    CHECK(resolveBlob(bytes, {8, 0}, 0, 4));
    CHECK(!resolveBlob(bytes, {9, 0}, 0));
    CHECK(!resolveBlob(bytes, {4, 5}, 100)); // must not silently truncate
    CHECK(!resolveBlob(bytes, {UINT64_MAX, 2}, SIZE_MAX));
    CHECK(!resolveBlob(bytes, {1, UINT64_MAX}, SIZE_MAX));
    CHECK(!resolveBlob(bytes, {1, 1}, 100, 4));
    CHECK(!resolveBlob(bytes, {0, 1}, 100, 3));
    CHECK(!resolveBlob(bytes, {0, 8}, 7));
    std::array attachments{bytes};
    CHECK(!resolveExternal(attachments, {1, 0, 0}, 100));
    CHECK(!resolveExternal(attachments, {0, 4, 5}, 100));
    fixture::Payload payload{bytes, {0, 0, 8}, {}};
    auto packet = encodeData(payload, attachments, kLimits);
    CHECK(packet);
    CHECK(!decodeData<fixture::Payload>({DataSchema<fixture::Payload>::describe(), *packet, {}}, kLimits));
    CHECK(!decodeData<fixture::Payload>({DataSchema<fixture::Payload>::describe(), *packet, attachments}, {100, 7}));
    for (auto offset : {std::uint64_t{0}, std::uint64_t{35}, UINT64_MAX})
    {
        auto data = mutableCopy(*packet);
        cxx::BinarySpanWriter writer(data);
        writer.writeUnsigned(offset);
        auto invalid = owned(data);
        CHECK(!validateEncodedData({DataSchema<fixture::Payload>::describe(), invalid, attachments}, kLimits));
    }
    auto data = mutableCopy(*packet);
    data.push_back(std::byte{0});
    auto trailing = owned(data);
    CHECK(!validateEncodedData({DataSchema<fixture::Payload>::describe(), trailing, attachments}, kLimits));
    CHECK(!cxx::checkedAdd<std::uint64_t>(UINT64_MAX, 1));
    CHECK(!cxx::checkedMul<std::uint64_t>(UINT64_MAX, 2));
    return 0;
}

static int ownershipTests()
{
    struct Counted final
    {
        int* destroyed;
        std::array<std::byte, 8> data{std::byte{42}};
        explicit Counted(int& count) : destroyed(&count) {}
        ~Counted() { ++*destroyed; }
    };
    int destroyed = 0;
    std::weak_ptr<Counted> weak;
    fixture::Payload retained;
    {
        auto source = std::make_shared<Counted>(destroyed);
        weak = source;
        auto owner = lux::cxx::SharedBytes<>::fromOwner(source, source->data);
        std::array attachments{owner};
        fixture::Payload input{owner, {0, 0, 8}, {}};
        auto packet = encodeData(input, attachments, kLimits);
        CHECK(packet);
        auto result = decodeData<fixture::Payload>(
            {DataSchema<fixture::Payload>::describe(), *packet, attachments}, kLimits
        );
        CHECK(result);
        retained = std::move(*result);
    }
    CHECK(destroyed == 0 && !weak.expired());
    CHECK(retained.blob.data()[0] == std::byte{42});
    auto subrange = resolveBlob(retained.external_bytes, {0, 1}, 1);
    CHECK(subrange);
    retained = {};
    CHECK(destroyed == 0);
    bool thread_read = false;
    std::thread consumer([bytes = std::move(*subrange), &thread_read]
    {
        auto copy = bytes;
        thread_read = copy.size() == 1 && copy.data()[0] == std::byte{42};
    });
    consumer.join();
    CHECK(thread_read && destroyed == 1 && weak.expired());
    return 0;
}

static int contractTests()
{
    using enum fixture::ECodecFault;
    for (auto fault : {SIZE, SHORT_WRITE, LONG_WRITE, ENCODE_ERROR})
    {
        fixture::codec_fault = fault;
        CHECK(!encodeData(fixture::Record{}, {}, kLimits));
    }
    fixture::codec_fault = NONE;
    auto bytes = encodeData(fixture::Record{}, {}, kLimits);
    CHECK(bytes);
    for (auto fault : {SHORT_READ, DECODE_ERROR})
    {
        fixture::codec_fault = fault;
        CHECK(!decodeData<fixture::Record>({DataSchema<fixture::Record>::describe(), *bytes, {}}, kLimits));
    }
    fixture::codec_fault = NONE;

    // Cover every supported scalar without introducing a third application Schema.
    static constexpr DataField fields[]{
        {"i8", cxx::EScalarKind::I8}, {"u8", cxx::EScalarKind::U8},
        {"i16", cxx::EScalarKind::I16}, {"u16", cxx::EScalarKind::U16},
        {"i32", cxx::EScalarKind::I32}, {"u32", cxx::EScalarKind::U32},
        {"i64", cxx::EScalarKind::I64}, {"u64", cxx::EScalarKind::U64},
        {"f32", cxx::EScalarKind::F32}, {"f64", cxx::EScalarKind::F64},
        {"bool", cxx::EScalarKind::BOOL}
    };
    static constexpr DataSchemaDescriptor scalar_schema{
        DataTypeIdView("test.g2.scalar-validation"), {1, 0}, {1, 0}, fields, 43, 1
    };
    std::array<std::byte, 43> data{};
    cxx::BinarySpanWriter writer(data);
    writer.writeSigned<std::int8_t>(INT8_MIN);
    writer.writeUnsigned<std::uint8_t>(UINT8_MAX);
    writer.writeSigned<std::int16_t>(INT16_MIN);
    writer.writeUnsigned<std::uint16_t>(UINT16_MAX);
    writer.writeSigned<std::int32_t>(INT32_MIN);
    writer.writeUnsigned<std::uint32_t>(UINT32_MAX);
    writer.writeSigned<std::int64_t>(INT64_MIN);
    writer.writeUnsigned<std::uint64_t>(UINT64_MAX);
    writer.writeFloat(1.0f);
    writer.writeFloat(0.5);
    writer.writeBool(true);
    auto packet = owned(data);
    CHECK(writer.good() && writer.size() == 43 && validateEncodedData({scalar_schema, packet, {}}, kLimits));
    cxx::BinarySpanWriter bad_float(std::span(data).subspan(34, 8));
    bad_float.writeUnsigned<std::uint64_t>(0x7ff0000000000001ULL);
    packet = owned(data);
    CHECK(!validateEncodedData({scalar_schema, packet, {}}, kLimits));

    static constexpr DataField blob_fields[]{
        {"flag", cxx::EScalarKind::U8},
        {"blobs", cxx::EScalarKind::U8, 2, EDataFieldKind::BLOB, 4}
    };
    static constexpr DataSchemaDescriptor blob_schema{
        DataTypeIdView("test.g2.blob-padding"), {1, 0}, {1, 0}, blob_fields, 33, 1
    };
    std::array<std::byte, 41> blob_data{};
    cxx::BinarySpanWriter blobs(blob_data);
    blobs.writeUnsigned<std::uint8_t>(0);
    blobs.writeUnsigned<std::uint64_t>(36);
    blobs.writeUnsigned<std::uint64_t>(1);
    blobs.writeUnsigned<std::uint64_t>(40);
    blobs.writeUnsigned<std::uint64_t>(1);
    packet = owned(blob_data);
    CHECK(validateEncodedData({blob_schema, packet, {}}, kLimits));
    blob_data[34] = std::byte{1};
    packet = owned(blob_data);
    CHECK(!validateEncodedData({blob_schema, packet, {}}, kLimits));
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test(argv[1]);
    if (test == "schema") return schemaTests();
    if (test == "roundtrip") return roundtripTests();
    if (test == "malformed") return malformedTests();
    if (test == "references") return referenceTests();
    if (test == "ownership") return ownershipTests();
    if (test == "contracts") return contractTests();
    return 2;
}
