#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/core/detail/ChangeJournal.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    struct Link final
    {
        int value{};
        lux::ecs::Entity target{lux::ecs::NullEntity};
        lux::ecs::PersistentEntityRef external;
    };

    struct AllocationProbe final
    {
    };

    lux::cxx::expected<void, lux::ecs::EComponentCodecError>
    encodeAllocationFailure(
        const lux::ecs::ComponentSchema&,
        const lux::ecs::World&,
        lux::ecs::Entity,
        lux::ecs::ComponentEncodePort&
    ) noexcept
    {
        return lux::cxx::unexpected(
            lux::ecs::EComponentCodecError::ALLOCATION_FAILURE
        );
    }

    lux::cxx::expected<void, lux::ecs::EComponentCodecError>
    decodeAllocationFailure(
        const lux::ecs::ComponentSchema&,
        lux::ecs::WorldEdit&,
        lux::ecs::Entity,
        std::uint32_t,
        lux::ecs::ComponentDecodePort&
    ) noexcept
    {
        return lux::cxx::unexpected(
            lux::ecs::EComponentCodecError::ALLOCATION_FAILURE
        );
    }

    lux::cxx::expected<void, lux::ecs::EComponentCodecError> encodeLink(
        const lux::ecs::ComponentSchema&,
        const lux::ecs::World& world,
        lux::ecs::Entity entity,
        lux::ecs::ComponentEncodePort& port
    ) noexcept
    {
        const Link& link = world.get<Link>(entity);
        std::array<std::byte, sizeof(int)> portable_value{};
        auto raw_value = static_cast<std::make_unsigned_t<int>>(link.value);
        for (std::size_t index{}; index < portable_value.size(); ++index)
        {
            portable_value[index] = static_cast<std::byte>(raw_value & 0xffU);
            raw_value >>= 8U;
        }
        if (auto value = port.write(
                "value",
                lux::ecs::EComponentWireType::SIGNED_INTEGER,
                portable_value); !value)
        {
            return value;
        }
        if (auto target = port.writeEntity("target", link.target); !target)
            return target;
        const auto stable = link.external.value.value.as_bytes();
        return port.writeStableReference("external", stable);
    }

    lux::cxx::expected<void, lux::ecs::EComponentCodecError> decodeLink(
        const lux::ecs::ComponentSchema&,
        lux::ecs::WorldEdit& edit,
        lux::ecs::Entity entity,
        std::uint32_t version,
        lux::ecs::ComponentDecodePort& port
    ) noexcept
    {
        if (version != 1)
            return lux::cxx::unexpected(lux::ecs::EComponentCodecError::UNSUPPORTED_VERSION);
        Link value;
        lux::ecs::EncodedPropertyView property;
        while (port.next(property))
        {
            if (property.name == "value" && property.bytes.size() == sizeof(int))
            {
                std::make_unsigned_t<int> raw_value{};
                for (std::size_t index{}; index < property.bytes.size(); ++index)
                {
                    raw_value |= static_cast<std::make_unsigned_t<int>>(
                        std::to_integer<unsigned int>(property.bytes[index])
                    ) << (index * 8U);
                }
                value.value = static_cast<int>(raw_value);
            }
            else if (property.name == "target")
            {
                auto target = port.resolveEntity(property.bytes);
                if (!target) return lux::cxx::unexpected(target.error());
                value.target = *target;
            }
            else if (property.name == "external")
            {
                auto stable = port.resolveStableReference(property.bytes);
                if (!stable)
                    return lux::cxx::unexpected(stable.error());
                std::array<std::uint8_t, 16> bytes{};
                std::memcpy(bytes.data(), stable->data(), bytes.size());
                value.external.value.value = uuids::uuid(bytes);
            }
        }
        edit.emplace<Link>(entity, value);
        return {};
    }

    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    template <class T>
        requires std::is_integral_v<T> && std::is_unsigned_v<T>
    void appendLittle(std::vector<std::byte>& destination, T value)
    {
        const std::size_t offset = destination.size();
        destination.resize(offset + sizeof(T));
        for (std::size_t index{}; index < sizeof(T); ++index)
        {
            destination[offset + index] = static_cast<std::byte>(
                value & static_cast<T>(0xffU)
            );
            if constexpr (sizeof(T) > 1U)
                value >>= 8U;
        }
    }
}

int main()
{
    std::vector<std::byte> tagged_bytes;
    std::vector<std::string> tagged_names;
    lux::ecs::TaggedPropertyWriter tagged_writer(tagged_bytes, tagged_names);
    const std::array tagged_value{std::byte{0xaa}, std::byte{0xbb}};
    assert(tagged_writer.write(
        "value",
        lux::ecs::EComponentWireType::UNSIGNED_INTEGER,
        tagged_value
    ));
    assert(tagged_writer.finish());
    const std::vector<std::byte> tagged_golden{
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01},
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xaa}, std::byte{0xbb}};
    assert(tagged_bytes == tagged_golden);
    lux::ecs::TaggedPropertyReader tagged_reader(tagged_bytes, tagged_names);
    lux::ecs::EncodedPropertyView tagged_property;
    assert(tagged_reader.next(tagged_property));
    assert(tagged_property.name == "value");
    assert(tagged_property.bytes.size() == 2);
    assert(!tagged_reader.next(tagged_property));
    assert(tagged_reader.valid());

    lux::ecs::WorldSectionImage empty_image;
    empty_image.id.value = uuid("10000000-0000-4000-8000-000000000001");
    auto empty_bytes = lux::ecs::encodeWorldSection(empty_image);
    assert(empty_bytes);
    std::vector<std::byte> empty_golden{
        std::byte{0x4c}, std::byte{0x58}, std::byte{0x57}, std::byte{0x53},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00},
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
    for (int index{}; index < 7; ++index)
        appendLittle(empty_golden, std::uint32_t{});
    assert(*empty_bytes == empty_golden);
    assert(lux::ecs::decodeWorldSection(empty_golden));

    const auto link_schema = lux::ecs::makeComponentSchema<Link>(
        lux::ecs::componentSchemaId("test.link"),
        1,
        lux::ecs::EComponentSnapshotPolicy::COPY,
        lux::ecs::ComponentCodec{&encodeLink, &decodeLink}
    );
    const auto allocation_schema =
        lux::ecs::makeComponentSchema<AllocationProbe>(
            lux::ecs::componentSchemaId("test.allocation"),
            1,
            lux::ecs::EComponentSnapshotPolicy::COPY,
            lux::ecs::ComponentCodec{
                &encodeAllocationFailure,
                &decodeAllocationFailure}
        );
    auto schemas = lux::ecs::ComponentSchemaSet::build(
        {
            lux::ecs::persistentIdComponentSchema(),
            link_schema,
            allocation_schema}
    );
    assert(schemas);

    lux::ecs::World world;
    auto edit_result = world.edit();
    auto edit = std::move(*edit_result);
    const auto first = edit.create();
    const auto second = edit.create();
    edit.emplace<lux::ecs::PersistentId>(first, lux::ecs::PersistentEntityId{uuid("00000000-0000-4000-8000-000000000002")});
    edit.emplace<lux::ecs::PersistentId>(second, lux::ecs::PersistentEntityId{uuid("00000000-0000-4000-8000-000000000001")});
    const lux::ecs::PersistentEntityId external_id{
        uuid("20000000-0000-4000-8000-000000000001")};
    edit.emplace<Link>(first, 7, second, lux::ecs::PersistentEntityRef{external_id});
    edit.emplace<Link>(second, 9, first, lux::ecs::PersistentEntityRef{external_id});
    edit.emplace<AllocationProbe>(first);
    edit = {};

    const std::array entities{first, second};
    const std::array selected{link_schema.id};
    auto busy_edit_result = world.edit();
    assert(busy_edit_result);
    auto busy_image = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        lux::ecs::WorldSectionId{
            uuid("10000000-0000-4000-8000-000000000001")},
        lux::ecs::WorldSectionWriteSelection{entities, selected}
    );
    assert(!busy_image);
    assert(busy_image.error().code == lux::ecs::EPersistenceError::WORLD_BUSY);
    auto busy_index = lux::ecs::PersistentEntityIndex::build(world);
    assert(!busy_index);
    assert(
        busy_index.error() ==
        lux::ecs::EPersistentEntityIndexError::WORLD_BUSY
    );
    auto busy_edit = std::move(*busy_edit_result);
    busy_edit = {};

    std::atomic_bool wrong_thread_build_rejected{};
    std::atomic_bool wrong_thread_index_rejected{};
    std::thread wrong_thread(
        [&]
        {
            auto built = lux::ecs::WorldSectionWriter::build(
                world,
                *schemas,
                lux::ecs::WorldSectionId{
                    uuid("10000000-0000-4000-8000-000000000001")},
                lux::ecs::WorldSectionWriteSelection{entities, selected}
            );
            wrong_thread_build_rejected.store(
                !built &&
                built.error().code == lux::ecs::EPersistenceError::WORLD_BUSY,
                std::memory_order_relaxed
            );
            auto index = lux::ecs::PersistentEntityIndex::build(world);
            wrong_thread_index_rejected.store(
                !index && index.error() ==
                    lux::ecs::EPersistentEntityIndexError::WORLD_BUSY,
                std::memory_order_relaxed
            );
        }
    );
    wrong_thread.join();
    assert(wrong_thread_build_rejected.load(std::memory_order_relaxed));
    assert(wrong_thread_index_rejected.load(std::memory_order_relaxed));

    auto image = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        lux::ecs::WorldSectionId{uuid("10000000-0000-4000-8000-000000000001")},
        lux::ecs::WorldSectionWriteSelection{entities, selected}
    );
    assert(image);
    assert(image->entities.size() == 2);
    assert(image->entities[0].id.value == uuid("00000000-0000-4000-8000-000000000001"));
    assert(image->entity_relocations.size() == 2);
    assert(image->persistent_relocations.size() == 2);
    assert(image->entity_relocations[0].payload_offset != 0);
    assert(image->persistent_relocations[0].payload_offset != 0);

    const std::array allocation_selected{allocation_schema.id};
    auto allocation_failure = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        lux::ecs::WorldSectionId{
            uuid("10000000-0000-4000-8000-000000000001")},
        lux::ecs::WorldSectionWriteSelection{entities, allocation_selected}
    );
    assert(!allocation_failure);
    assert(
        allocation_failure.error().code ==
        lux::ecs::EPersistenceError::ALLOCATION_FAILURE
    );

    auto bytes = lux::ecs::encodeWorldSection(*image);
    assert(bytes);
    auto bytes_again = lux::ecs::encodeWorldSection(*image);
    assert(bytes_again && *bytes == *bytes_again);

    auto decoded = lux::ecs::decodeWorldSection(*bytes);
    assert(decoded);
    auto reencoded = lux::ecs::encodeWorldSection(*decoded);
    assert(reencoded && *reencoded == *bytes);

    lux::ecs::WorldSectionLimits small_limits;
    small_limits.max_entities = 1;
    assert(!lux::ecs::decodeWorldSection(*bytes, small_limits));

    auto corrupt_relocation = *decoded;
    ++corrupt_relocation.entity_relocations[0].payload_offset;
    auto corrupt_result = lux::ecs::encodeWorldSection(corrupt_relocation);
    assert(!corrupt_result);
    assert(corrupt_result.error().code == lux::ecs::EPersistenceError::INVALID_RELOCATION);

    auto future_field = *decoded;
    const std::uint32_t future_name =
        static_cast<std::uint32_t>(future_field.property_names.size());
    future_field.property_names.emplace_back("future_field");
    auto& future_payload = future_field.columns[0].cells[0].payload;
    future_payload[0] = static_cast<std::byte>(
        std::to_integer<std::uint8_t>(future_payload[0]) + 1U
    );
    appendLittle(future_payload, future_name);
    appendLittle(
        future_payload,
        static_cast<std::uint8_t>(lux::ecs::EComponentWireType::BYTES)
    );
    const std::array<std::byte, 3> future_bytes{
        std::byte{1}, std::byte{2}, std::byte{3}};
    appendLittle(
        future_payload, static_cast<std::uint32_t>(future_bytes.size())
    );
    future_payload.insert(
        future_payload.end(),
        future_bytes.begin(),
        future_bytes.end()
    );
    auto future_loaded = lux::ecs::WorldSectionReader::materialize(
        future_field,
        *schemas
    );
    assert(future_loaded);

    auto unknown_schema = *decoded;
    unknown_schema.schemas[0].id = lux::ecs::componentSchemaId("future.link");
    auto unknown_bytes = lux::ecs::encodeWorldSection(unknown_schema);
    assert(unknown_bytes);
    auto preserved_unknown = lux::ecs::decodeWorldSection(*unknown_bytes);
    assert(preserved_unknown);
    assert(preserved_unknown->columns[0].cells[0].payload ==
        unknown_schema.columns[0].cells[0].payload);
    auto missing_schema = lux::ecs::WorldSectionReader::materialize(
        *preserved_unknown,
        *schemas
    );
    assert(!missing_schema);
    assert(missing_schema.error().code == lux::ecs::EPersistenceError::MISSING_SCHEMA);

    auto future_version = *decoded;
    future_version.schemas[0].version = 2;
    auto unsupported = lux::ecs::WorldSectionReader::materialize(
        future_version,
        *schemas
    );
    assert(!unsupported);
    assert(unsupported.error().code ==
        lux::ecs::EPersistenceError::COMPONENT_DECODE_FAILED);

    auto allocation_link_schema = link_schema;
    allocation_link_schema.codec.decode = &decodeAllocationFailure;
    auto allocation_decode_schemas = lux::ecs::ComponentSchemaSet::build(
        {
            lux::ecs::persistentIdComponentSchema(),
            allocation_link_schema}
    );
    assert(allocation_decode_schemas);
    auto allocation_decode = lux::ecs::WorldSectionReader::materialize(
        *decoded, *allocation_decode_schemas
    );
    assert(!allocation_decode);
    assert(
        allocation_decode.error().code ==
        lux::ecs::EPersistenceError::ALLOCATION_FAILURE
    );

    const lux::ecs::WorldConfig bounded_config{
        lux::ecs::ChangeJournalConfig{4096U, 4096U}};
    auto loaded = lux::ecs::WorldSectionReader::materialize(
        *decoded, *schemas, bounded_config
    );
    assert(loaded);
    lux::ecs::ChangeCursor<Link> loaded_cursor;
    auto& loaded_journal =
        lux::ecs::detail::WorldChangeAccess::journal(**loaded);
    assert(loaded_journal.recordWriteCountForTest() == 0U);
    assert(loaded_journal.dynamicBlockAcquisitionsForTest() == 0U);
    assert(
        loaded_journal.read(loaded_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    auto index = lux::ecs::PersistentEntityIndex::build(**loaded);
    assert(index && index->size() == 2);
    const auto loaded_first = index->find(lux::ecs::PersistentEntityId{uuid("00000000-0000-4000-8000-000000000002")});
    const auto loaded_second = index->find(lux::ecs::PersistentEntityId{uuid("00000000-0000-4000-8000-000000000001")});
    assert((**loaded).get<Link>(loaded_first).value == 7);
    assert((**loaded).get<Link>(loaded_first).target == loaded_second);
    assert((**loaded).get<Link>(loaded_first).external.value == external_id);

    auto loaded_edit_result = (*loaded)->edit();
    auto loaded_edit = std::move(*loaded_edit_result);
    for (int iteration{}; iteration < 512; ++iteration)
    {
        loaded_edit.update<Link>(
            loaded_first,
            [](Link& link) noexcept
            {
                ++link.value;
            }
        );
    }
    loaded_edit = {};
    assert(
        loaded_journal.read(loaded_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    auto truncated = *bytes;
    truncated.pop_back();
    auto truncated_result = lux::ecs::decodeWorldSection(truncated);
    assert(!truncated_result);
    assert(
        truncated_result.error().code == lux::ecs::EPersistenceError::TRUNCATED
    );

    const std::array partial{first};
    auto bad_reference = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        lux::ecs::WorldSectionId{uuid("10000000-0000-4000-8000-000000000001")},
        lux::ecs::WorldSectionWriteSelection{partial, selected}
    );
    assert(!bad_reference);
    assert(bad_reference.error().code == lux::ecs::EPersistenceError::ENTITY_REFERENCE_OUTSIDE_SECTION);
}
