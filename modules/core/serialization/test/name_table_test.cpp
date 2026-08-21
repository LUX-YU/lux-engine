#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <cassert>
#include <array>
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

    std::vector<std::byte> bytes;
    lux::serialize::ArchiveWriter writer{bytes};
    writer.writePod<std::uint32_t>(17u);
    writer.writeString("core-only");
    writer.writeUuid(uuids::uuid{});

    lux::serialize::ArchiveReader reader{bytes.data(), bytes.size()};
    assert(reader.readPod<std::uint32_t>() == 17u);
    assert(reader.readString() == "core-only");
    assert(reader.readUuid().is_nil());
    assert(reader.eof());

    const std::array<std::byte, 3> borrowed{
        std::byte{1}, std::byte{2}, std::byte{3}};
    lux::serialize::ArchiveReader span_reader{borrowed.data(), borrowed.size()};
    const auto prefix = span_reader.readSpan(2u);
    assert(prefix.size() == 2u);
    assert(prefix[0] == std::byte{1});
    assert(span_reader.remaining() == 1u);
    assert(span_reader.readSpan(2u).empty());
    assert(!span_reader.ok());
    assert(span_reader.readPod<std::uint8_t>() == 0u);
    assert(span_reader.remainingSpan().empty());

    const std::array<std::byte, 4> zero_count{};
    lux::serialize::ArchiveReader invalid_table_reader{
        zero_count.data(), zero_count.size()};
    [[maybe_unused]] const auto invalid_table =
        lux::serialize::NameTable::deserialize(invalid_table_reader);
    assert(!invalid_table_reader.ok());

    std::vector<std::byte> truncated_table_bytes;
    lux::serialize::ArchiveWriter truncated_table_writer{
        truncated_table_bytes};
    truncated_table_writer.writePod<std::uint32_t>(2u);
    lux::serialize::ArchiveReader truncated_table_reader{
        truncated_table_bytes.data(), truncated_table_bytes.size()};
    [[maybe_unused]] const auto truncated_table =
        lux::serialize::NameTable::deserialize(truncated_table_reader);
    assert(!truncated_table_reader.ok());
    return 0;
}
