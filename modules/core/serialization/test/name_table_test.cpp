#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

int main()
{
    lux::serialize::NameTable names;
    constexpr std::uint32_t kNameCount = 8192u;
    std::vector<std::string> source;
    std::vector<std::uint32_t> indices;
    source.reserve(kNameCount);
    indices.reserve(kNameCount);

    for (std::uint32_t index = 0u; index < kNameCount; ++index)
    {
        // Most entries remain inside std::string's small-string buffer. This
        // forces many storage growth steps while index_ retains their views.
        source.push_back("f" + std::to_string(index));
        indices.push_back(names.intern(source.back()));
        assert(indices.back() == index + 1u);
    }

    for (std::uint32_t index = 0u; index < kNameCount; ++index)
    {
        assert(names.intern(source[index]) == indices[index]);
        assert(names.at(indices[index]) == source[index]);
    }
    assert(names.intern({}) == 0u);
    assert(names.at(0u).empty());

    // A cooked relocation remains reflected so the owning format can
    // validate its exact destination, but it must never leak into the
    // domain-neutral tagged-property archive.
    struct RelocationFixture final
    {
        std::uint32_t value{0u};
        std::uint32_t relocated{0u};
    };
    lux::meta::RefClass fixture_class;
    fixture_class.fields = {
        lux::meta::RefField{
            .name = "value",
            .type = lux::meta::ref_type_of_v<std::uint32_t>,
            .offset = static_cast<std::uint32_t>(
                offsetof(RelocationFixture, value))},
        lux::meta::RefField{
            .name = "relocated",
            .type = lux::meta::ref_type_of_v<std::uint32_t>,
            .offset = static_cast<std::uint32_t>(
                offsetof(RelocationFixture, relocated)),
            .annotation_str =
                "luxref::property::member, cooked_relocation = content_blob_ref"}};

    const RelocationFixture source_value{17u, 99u};
    std::vector<std::byte> payload;
    lux::serialize::NameTable payload_names;
    lux::serialize::ArchiveWriter payload_writer{payload};
    lux::serialize::TaggedPropertyWriter tagged_writer{
        payload_writer, payload_names};
    tagged_writer.writeObject(fixture_class, &source_value);
    assert(payload_names.size() == 2u);
    assert(payload_names.at(1u) == "value");

    RelocationFixture decoded{0u, 41u};
    lux::serialize::ArchiveReader payload_reader{
        payload.data(), payload.size()};
    lux::serialize::TaggedPropertyReader tagged_reader{
        payload_reader, payload_names};
    assert(tagged_reader.readObjectExact(fixture_class, &decoded));
    assert(payload_reader.eof());
    assert(decoded.value == source_value.value);
    assert(decoded.relocated == 41u);
    return 0;
}
