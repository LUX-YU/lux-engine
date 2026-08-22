#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>

#include <lux/engine/meta/Meta.hpp>

#include <cassert>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct Fixture final
    {
        std::uint32_t number{};
        std::string   label;
        uuids::uuid   identifier{};
        std::uint32_t relocated{};
    };

    lux::meta::RefClass fixtureClass()
    {
        lux::meta::RefClass reflected;
        reflected.fields = {
            lux::meta::RefField{
                .name = "number",
                .type = lux::meta::ref_type_of_v<std::uint32_t>,
                .offset = static_cast<std::uint32_t>(offsetof(Fixture, number)),
            },
            lux::meta::RefField{
                .name = "label",
                .type = lux::meta::ref_type_of_v<std::string>,
                .offset = static_cast<std::uint32_t>(offsetof(Fixture, label)),
            },
            lux::meta::RefField{
                .name = "identifier",
                .type = lux::meta::ref_type_of_v<uuids::uuid>,
                .offset = static_cast<std::uint32_t>(offsetof(Fixture, identifier)),
            },
            lux::meta::RefField{
                .name = "relocated",
                .type = lux::meta::ref_type_of_v<std::uint32_t>,
                .offset = static_cast<std::uint32_t>(offsetof(Fixture, relocated)),
                .annotation_str =
                    "luxref::property::member, cooked_relocation=content_blob_ref",
            },
        };
        return reflected;
    }

    struct NestedValue final
    {
        std::uint32_t first{};
        std::uint32_t second{};
    };

    struct NestedFixture final
    {
        NestedValue nested;
        std::uint32_t tail{};
    };

    lux::meta::RefClass nestedValueClass()
    {
        lux::meta::RefClass reflected;
        reflected.fields = {
            lux::meta::RefField{
                .name = "first",
                .type = lux::meta::ref_type_of_v<std::uint32_t>,
                .offset = static_cast<std::uint32_t>(
                    offsetof(NestedValue, first)),
            },
            lux::meta::RefField{
                .name = "second",
                .type = lux::meta::ref_type_of_v<std::uint32_t>,
                .offset = static_cast<std::uint32_t>(
                    offsetof(NestedValue, second)),
            },
        };
        return reflected;
    }

    lux::meta::RefClass nestedFixtureClass(
        lux::meta::RefClass& nested_class)
    {
        auto nested_type = lux::meta::ref_type_of_v<NestedValue>;
        nested_type.ptr = &nested_class;
        lux::meta::RefClass reflected;
        reflected.fields = {
            lux::meta::RefField{
                .name = "nested",
                .type = nested_type,
                .offset = static_cast<std::uint32_t>(
                    offsetof(NestedFixture, nested)),
            },
            lux::meta::RefField{
                .name = "tail",
                .type = lux::meta::ref_type_of_v<std::uint32_t>,
                .offset = static_cast<std::uint32_t>(
                    offsetof(NestedFixture, tail)),
            },
        };
        return reflected;
    }
}

int main()
{
    using namespace lux::ecs::serialization;

    static_assert(static_cast<std::uint8_t>(EArchiveType::Uuid) == 48u);
    static_assert(static_cast<std::uint8_t>(EArchiveType::Struct) == 64u);

    auto reflected = fixtureClass();
    const Fixture source{
        .number = 17u,
        .label = "hello",
        .identifier = uuids::uuid{
            std::array<std::uint8_t, 16>{
                1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
                9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u}},
        .relocated = 99u,
    };

    lux::serialize::NameTable names;
    std::vector<std::byte> payload;
    lux::serialize::ArchiveWriter archive_writer{payload};
    TaggedPropertyWriter writer{archive_writer, names};
    const auto written = writer.writeObject(reflected, &source);
    assert(written);
    assert(payload.size() == 60u);
    assert(names.size() == 4u);
    assert(payload[35] == std::byte{48u});

    const auto framed = validateTaggedPropertyObject(payload, names.size());
    assert(framed);

    Fixture exact_value{.relocated = 41u};
    lux::serialize::ArchiveReader exact_archive{payload.data(), payload.size()};
    TaggedPropertyReader exact_reader{exact_archive, names};
    const auto exact = exact_reader.readObjectExact(reflected, &exact_value);
    assert(exact);
    assert(exact_archive.eof());
    assert(exact_value.number == source.number);
    assert(exact_value.label == source.label);
    assert(exact_value.identifier == source.identifier);
    assert(exact_value.relocated == 41u);

    const auto future_name = names.intern("future");
    std::vector<std::byte> compatible_payload;
    lux::serialize::ArchiveWriter compatible_writer{compatible_payload};
    compatible_writer.writePod(future_name);
    compatible_writer.writePod<std::uint8_t>(200u);
    compatible_writer.writePod<std::uint32_t>(3u);
    const std::byte future_bytes[3]{
        std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
    compatible_writer.writeBytes(future_bytes, sizeof(future_bytes));
    compatible_writer.writeBytes(payload.data(), payload.size());

    Fixture compatible_value{.relocated = 43u};
    lux::serialize::ArchiveReader compatible_archive{
        compatible_payload.data(), compatible_payload.size()};
    TaggedPropertyReader compatible_reader{compatible_archive, names};
    assert(compatible_reader.readObject(reflected, &compatible_value));
    assert(compatible_value.number == source.number);
    assert(compatible_value.relocated == 43u);

    Fixture rejected_exact{};
    lux::serialize::ArchiveReader noncanonical_archive{
        compatible_payload.data(), compatible_payload.size()};
    TaggedPropertyReader noncanonical_reader{noncanonical_archive, names};
    const auto noncanonical =
        noncanonical_reader.readObjectExact(reflected, &rejected_exact);
    assert(!noncanonical);
    assert(noncanonical.error().error ==
           EComponentArchiveError::NON_CANONICAL_OBJECT);

    std::vector<std::byte> trailing = payload;
    trailing.push_back(std::byte{0x7F});
    assert(!validateTaggedPropertyObject(trailing, names.size()));

    std::vector<std::byte> truncated = payload;
    truncated.pop_back();
    const auto truncated_result =
        validateTaggedPropertyObject(truncated, names.size());
    assert(!truncated_result);
    assert(truncated_result.error().error ==
           EComponentArchiveError::TRUNCATED);

    std::vector<std::byte> bad_name = payload;
    const auto bad_index = names.size();
    std::memcpy(bad_name.data(), &bad_index, sizeof(bad_index));
    const auto bad_name_result =
        validateTaggedPropertyObject(bad_name, names.size());
    assert(!bad_name_result);
    assert(bad_name_result.error().error ==
           EComponentArchiveError::INVALID_NAME_INDEX);

    std::vector<std::byte> duplicate{
        payload.begin(), payload.end() - sizeof(std::uint32_t)};
    duplicate.insert(
        duplicate.end(), payload.begin(), payload.begin() + 13u);
    lux::serialize::ArchiveWriter duplicate_writer{duplicate};
    duplicate_writer.writePod(kEndOfObject);
    Fixture duplicate_value{};
    lux::serialize::ArchiveReader duplicate_archive{
        duplicate.data(), duplicate.size()};
    TaggedPropertyReader duplicate_reader{duplicate_archive, names};
    const auto duplicate_result =
        duplicate_reader.readObject(reflected, &duplicate_value);
    assert(!duplicate_result);
    assert(duplicate_result.error().error ==
           EComponentArchiveError::NON_CANONICAL_OBJECT);

    struct Unsupported final
    {
        std::string_view view;
    };
    lux::meta::RefClass unsupported_class;
    unsupported_class.fields = {lux::meta::RefField{
        .name = "view",
        .type = lux::meta::ref_type_of_v<std::string_view>,
        .offset = static_cast<std::uint32_t>(offsetof(Unsupported, view)),
    }};
    const Unsupported unsupported{"borrowed"};
    std::vector<std::byte> untouched{std::byte{0x55}};
    lux::serialize::NameTable untouched_names;
    lux::serialize::ArchiveWriter untouched_archive{untouched};
    TaggedPropertyWriter unsupported_writer{
        untouched_archive, untouched_names};
    const auto unsupported_result =
        unsupported_writer.writeObject(unsupported_class, &unsupported);
    assert(!unsupported_result);
    assert(unsupported_result.error().error ==
           EComponentArchiveError::UNSUPPORTED_FIELD_TYPE);
    assert(untouched.size() == 1u);
    assert(untouched_names.size() == 1u);
    assert(unsupported_result.error().field_path == "view");

    struct FloatFixture final
    {
        float value{};
    };
    lux::meta::RefClass float_class;
    float_class.fields = {lux::meta::RefField{
        .name = "value",
        .type = lux::meta::ref_type_of_v<float>,
        .offset = static_cast<std::uint32_t>(offsetof(FloatFixture, value)),
    }};
    const FloatFixture invalid_float{
        std::numeric_limits<float>::quiet_NaN()};
    std::vector<std::byte> invalid_float_bytes;
    lux::serialize::NameTable invalid_float_names;
    lux::serialize::ArchiveWriter invalid_float_archive{invalid_float_bytes};
    TaggedPropertyWriter invalid_float_writer{
        invalid_float_archive, invalid_float_names};
    const auto invalid_float_result =
        invalid_float_writer.writeObject(float_class, &invalid_float);
    assert(!invalid_float_result);
    assert(invalid_float_result.error().error ==
           EComponentArchiveError::INVALID_VALUE);
    assert(invalid_float_bytes.empty());
    assert(invalid_float_names.size() == 1u);

    std::vector<std::byte> limited_bytes;
    lux::serialize::NameTable limited_names;
    lux::serialize::ArchiveWriter limited_archive{limited_bytes};
    ComponentArchiveLimits limits;
    limits.max_string_bytes = 4u;
    TaggedPropertyWriter limited_writer{limited_archive, limited_names, limits};
    const auto limited = limited_writer.writeObject(reflected, &source);
    assert(!limited);
    assert(limited.error().error == EComponentArchiveError::LIMIT_EXCEEDED);
    assert(limited_bytes.empty());
    assert(limited_names.size() == 1u);

    auto nested_class = nestedValueClass();
    auto nested_fixture_class = nestedFixtureClass(nested_class);
    const NestedFixture nested_source{
        .nested = {.first = 11u, .second = 22u},
        .tail = 33u,
    };
    lux::serialize::NameTable nested_names;
    std::vector<std::byte> nested_bytes;
    lux::serialize::ArchiveWriter nested_archive_writer{nested_bytes};
    TaggedPropertyWriter nested_writer{
        nested_archive_writer, nested_names};
    assert(nested_writer.writeObject(
        nested_fixture_class, &nested_source));

    NestedFixture nested_value{};
    lux::serialize::ArchiveReader nested_archive_reader{
        nested_bytes.data(), nested_bytes.size()};
    TaggedPropertyReader nested_reader{
        nested_archive_reader, nested_names};
    assert(nested_reader.readObjectExact(
        nested_fixture_class, &nested_value));
    assert(nested_value.nested.first == nested_source.nested.first);
    assert(nested_value.nested.second == nested_source.nested.second);
    assert(nested_value.tail == nested_source.tail);

    std::vector<std::byte> short_nested = nested_bytes;
    std::uint32_t nested_payload_size{};
    std::memcpy(
        &nested_payload_size,
        short_nested.data() + sizeof(std::uint32_t) + sizeof(std::uint8_t),
        sizeof(nested_payload_size));
    assert(nested_payload_size > 0u);
    --nested_payload_size;
    std::memcpy(
        short_nested.data() + sizeof(std::uint32_t) + sizeof(std::uint8_t),
        &nested_payload_size,
        sizeof(nested_payload_size));
    const auto bounded_nested = validateTaggedPropertyObject(
        short_nested, nested_names.size());
    assert(!bounded_nested);
    assert(bounded_nested.error().error ==
               EComponentArchiveError::TRUNCATED ||
           bounded_nested.error().error ==
               EComponentArchiveError::TRAILING_BYTES);

    const lux::meta::RefField opaque_uuid{
        .name = "opaque",
        .type = lux::meta::ref_type_of_v<uuids::uuid>,
    };
    const lux::meta::RefField texture_uuid{
        .name = "texture",
        .type = lux::meta::ref_type_of_v<uuids::uuid>,
        .annotation_str = "luxref::property::member, asset_type=texture",
    };
    assert(!isAssetReferenceField(opaque_uuid));
    assert(isAssetReferenceField(texture_uuid));
    return 0;
}
