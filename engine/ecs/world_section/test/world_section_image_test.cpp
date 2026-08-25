#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>

#include <uuid.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <vector>

namespace
{
    [[nodiscard]] lux::ecs::WorldSectionId sectionId()
    {
        return lux::ecs::WorldSectionId{
            uuids::uuid::from_string(
                "10000000-0000-4000-8000-000000000001"
            ).value()};
    }

    [[nodiscard]] std::vector<std::byte> fixedPayload(
        std::size_t row_count,
        std::size_t stride
    )
    {
        std::vector<std::byte> result(row_count * stride);
        for (std::size_t index{}; index < result.size(); ++index)
            result[index] = static_cast<std::byte>(index & 0xffU);
        return result;
    }
}

int main()
{
    using namespace lux::ecs;
    using namespace lux::ecs::world_section::test;

    auto empty_bytes = buildFixture(sectionId(), 0U, {});
    auto empty = WorldSectionImage::open(std::move(empty_bytes));
    assert(empty);
    assert(empty->id() == sectionId());
    assert(empty->entityCount() == 0U);
    assert(empty->columns().empty());
    assert(empty->bytes().size() == WorldSectionHeaderBytes);

    FixtureColumn dense_fixed;
    dense_fixed.schema_name = "test.DenseFixed";
    dense_fixed.value_encoding = EWorldSectionValueEncoding::FIXED;
    dense_fixed.fixed_stride = 8U;
    dense_fixed.payload = fixedPayload(3U, 8U);

    FixtureColumn sparse_variable;
    sparse_variable.schema_name = "test.SparseVariable";
    sparse_variable.value_encoding = EWorldSectionValueEncoding::VARIABLE;
    sparse_variable.ordinal_encoding =
        EWorldSectionOrdinalEncoding::U32_LIST;
    sparse_variable.ordinals = {0U, 2U};
    sparse_variable.offsets = {0U, 2U, 5U};
    sparse_variable.payload = {
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};

    FixtureColumn sparse_tag;
    sparse_tag.schema_name = "test.SparseTag";
    sparse_tag.ordinal_encoding = EWorldSectionOrdinalEncoding::U32_LIST;
    sparse_tag.ordinals = {1U};

    auto bytes = buildFixture(
        sectionId(),
        3U,
        {dense_fixed, sparse_variable, sparse_tag}
    );
    const auto original_size = bytes.size();
    auto image = WorldSectionImage::open(std::move(bytes));
    assert(image);
    assert(image->entityCount() == 3U);
    assert(image->columns().size() == 3U);
    assert(image->bytes().size() == original_size);
    bool found_fixed{};
    bool found_variable{};
    bool found_tag{};
    for (const auto& column : image->columns())
    {
        if (column.schemaName() == "test.DenseFixed")
        {
            found_fixed = true;
            assert(column.rowCount() == 3U);
            assert(column.fixedStride() == 8U);
            assert(column.ordinalBytes().empty());
            assert(column.payload().size() == 24U);
        }
        else if (column.schemaName() == "test.SparseVariable")
        {
            found_variable = true;
            assert(column.rowCount() == 2U);
            assert(column.ordinalBytes().size() == 8U);
            assert(column.offsetBytes().size() == 12U);
            assert(column.payload().size() == 5U);
        }
        else if (column.schemaName() == "test.SparseTag")
        {
            found_tag = true;
            assert(column.payload().empty());
        }
    }
    assert(found_fixed && found_variable && found_tag);

    const std::array v1_magic{
        std::byte{'L'}, std::byte{'X'}, std::byte{'W'}, std::byte{'C'},
        std::byte{1}, std::byte{}, std::byte{}, std::byte{}};
    auto v1 = WorldSectionImage::open(
        std::vector<std::byte>(v1_magic.begin(), v1_magic.end())
    );
    assert(!v1);
    assert(v1.error().code == EWorldSectionError::TRUNCATED);

    auto bad_version = buildFixture(sectionId(), 0U, {});
    patchU32(bad_version, 4U, 1U);
    auto unsupported = WorldSectionImage::open(std::move(bad_version));
    assert(!unsupported);
    assert(
        unsupported.error().code ==
        EWorldSectionError::UNSUPPORTED_FORMAT_VERSION
    );

    auto truncated = buildFixture(sectionId(), 0U, {});
    truncated.pop_back();
    auto truncated_result = WorldSectionImage::open(std::move(truncated));
    assert(!truncated_result);
    assert(truncated_result.error().code == EWorldSectionError::TRUNCATED);

    auto duplicate_ordinals = buildFixture(
        sectionId(),
        3U,
        {sparse_variable}
    );
    std::uint64_t ordinal_offset{};
    for (std::size_t index{}; index < sizeof(ordinal_offset); ++index)
    {
        ordinal_offset |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(
                duplicate_ordinals[80U + sparse_variable.schema_name.size() +
                                   32U + index]
            )
        ) << (index * 8U);
    }
    patchU32(
        duplicate_ordinals,
        static_cast<std::size_t>(ordinal_offset) + 4U,
        0U
    );
    auto duplicate = WorldSectionImage::open(std::move(duplicate_ordinals));
    assert(!duplicate);
    assert(duplicate.error().code == EWorldSectionError::INVALID_ORDINAL);

    auto bad_offsets = buildFixture(sectionId(), 3U, {sparse_variable});
    std::uint64_t offsets_offset{};
    for (std::size_t index{}; index < sizeof(offsets_offset); ++index)
    {
        offsets_offset |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(
                bad_offsets[80U + sparse_variable.schema_name.size() +
                            48U + index]
            )
        ) << (index * 8U);
    }
    patchU32(
        bad_offsets,
        static_cast<std::size_t>(offsets_offset) + 4U,
        6U
    );
    auto invalid_offsets = WorldSectionImage::open(std::move(bad_offsets));
    assert(!invalid_offsets);
    assert(
        invalid_offsets.error().code == EWorldSectionError::INVALID_OFFSETS
    );
}
