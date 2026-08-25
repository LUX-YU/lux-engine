#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/WorldSectionContract.hpp>

#include <lux/cxx/core/StableNameId.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <span>

namespace lux::ecs::world_section::test
{
    namespace
    {
        template <class Integer>
        void appendLittle(std::vector<std::byte>& bytes, Integer value)
        {
            for (std::size_t index{}; index < sizeof(Integer); ++index)
            {
                bytes.push_back(static_cast<std::byte>(value & 0xffU));
                value >>= 8U;
            }
        }

        template <class Integer>
        void patchLittle(
            std::vector<std::byte>& bytes,
            std::size_t offset,
            Integer value
        )
        {
            assert(offset <= bytes.size());
            assert(sizeof(Integer) <= bytes.size() - offset);
            for (std::size_t index{}; index < sizeof(Integer); ++index)
            {
                bytes[offset + index] = static_cast<std::byte>(value & 0xffU);
                value >>= 8U;
            }
        }

        void appendZeros(std::vector<std::byte>& bytes, std::size_t count)
        {
            bytes.insert(bytes.end(), count, std::byte{});
        }
    } // namespace

    std::vector<std::byte> buildFixture(
        WorldSectionId id,
        std::uint32_t entity_count,
        std::vector<FixtureColumn> columns
    )
    {
        std::sort(
            columns.begin(),
            columns.end(),
            [](const FixtureColumn& left, const FixtureColumn& right)
            {
                const auto left_hash = lux::cxx::Fnv1a64::hash(
                    left.schema_name
                );
                const auto right_hash = lux::cxx::Fnv1a64::hash(
                    right.schema_name
                );
                return left_hash < right_hash ||
                    (left_hash == right_hash &&
                     left.schema_name < right.schema_name);
            }
        );

        std::vector<std::byte> bytes(WorldSectionHeaderBytes);
        const std::uint64_t name_offset = bytes.size();
        std::vector<std::uint32_t> name_offsets;
        name_offsets.reserve(columns.size());
        for (const auto& column : columns)
        {
            name_offsets.push_back(static_cast<std::uint32_t>(
                bytes.size() - name_offset
            ));
            const auto* first = reinterpret_cast<const std::byte*>(
                column.schema_name.data()
            );
            bytes.insert(
                bytes.end(),
                first,
                first + column.schema_name.size()
            );
        }
        const std::uint64_t name_bytes = bytes.size() - name_offset;
        const std::uint64_t column_offset = bytes.size();
        appendZeros(
            bytes,
            columns.size() * WorldSectionColumnDescriptorBytes
        );

        for (std::size_t index{}; index < columns.size(); ++index)
        {
            const auto& column = columns[index];
            const std::uint32_t row_count =
                column.ordinal_encoding ==
                    EWorldSectionOrdinalEncoding::DENSE
                ? entity_count
                : static_cast<std::uint32_t>(column.ordinals.size());
            const std::uint64_t ordinals_offset = bytes.size();
            for (const std::uint32_t ordinal : column.ordinals)
                appendLittle(bytes, ordinal);
            const std::uint64_t ordinals_bytes =
                bytes.size() - ordinals_offset;
            const std::uint64_t offsets_offset = bytes.size();
            for (const std::uint32_t offset : column.offsets)
                appendLittle(bytes, offset);
            const std::uint64_t offsets_bytes = bytes.size() - offsets_offset;
            const std::uint64_t payload_offset = bytes.size();
            bytes.insert(
                bytes.end(),
                column.payload.begin(),
                column.payload.end()
            );
            const std::uint64_t payload_bytes = bytes.size() - payload_offset;

            const std::size_t descriptor = static_cast<std::size_t>(
                column_offset + index * WorldSectionColumnDescriptorBytes
            );
            patchU64(
                bytes,
                descriptor,
                lux::cxx::Fnv1a64::hash(column.schema_name)
            );
            patchU32(bytes, descriptor + 8U, column.schema_version);
            bytes[descriptor + 12U] = static_cast<std::byte>(
                column.value_encoding
            );
            bytes[descriptor + 13U] = static_cast<std::byte>(
                column.ordinal_encoding
            );
            patchU32(bytes, descriptor + 16U, name_offsets[index]);
            patchU32(
                bytes,
                descriptor + 20U,
                static_cast<std::uint32_t>(column.schema_name.size())
            );
            patchU32(bytes, descriptor + 24U, row_count);
            patchU32(bytes, descriptor + 28U, column.fixed_stride);
            patchU64(bytes, descriptor + 32U, ordinals_offset);
            patchU64(bytes, descriptor + 40U, ordinals_bytes);
            patchU64(bytes, descriptor + 48U, offsets_offset);
            patchU64(bytes, descriptor + 56U, offsets_bytes);
            patchU64(bytes, descriptor + 64U, payload_offset);
            patchU64(bytes, descriptor + 72U, payload_bytes);
        }

        bytes[0] = std::byte{'L'};
        bytes[1] = std::byte{'X'};
        bytes[2] = std::byte{'W'};
        bytes[3] = std::byte{'C'};
        patchU32(bytes, 4U, 2U);
        patchU32(bytes, 8U, 1U);
        patchU32(bytes, 12U, WorldSectionHeaderBytes);
        const auto id_bytes = id.value.as_bytes();
        for (std::size_t index{}; index < id_bytes.size(); ++index)
            bytes[16U + index] = static_cast<std::byte>(id_bytes[index]);
        patchU32(bytes, 32U, entity_count);
        patchU32(
            bytes,
            36U,
            static_cast<std::uint32_t>(columns.size())
        );
        patchU64(bytes, 40U, name_offset);
        patchU64(bytes, 48U, name_bytes);
        patchU64(bytes, 56U, column_offset);
        patchU64(
            bytes,
            64U,
            columns.size() * WorldSectionColumnDescriptorBytes
        );
        patchU64(bytes, 72U, bytes.size());
        return bytes;
    }

    void patchU32(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint32_t value
    )
    {
        patchLittle(bytes, offset, value);
    }

    void patchU64(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint64_t value
    )
    {
        patchLittle(bytes, offset, value);
    }
} // namespace lux::ecs::world_section::test
