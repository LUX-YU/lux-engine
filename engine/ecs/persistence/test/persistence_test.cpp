#include <lux/engine/ecs/WorldSection.hpp>

#include <array>
#include <cassert>
#include <cstring>
#include <span>
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

    lux::cxx::expected<void, lux::ecs::EComponentCodecError> encodeLink(
        const lux::ecs::ComponentSchema&,
        const lux::ecs::World& world,
        lux::ecs::Entity entity,
        lux::ecs::ComponentEncodePort& port
    ) noexcept
    {
        const Link& link = world.get<Link>(entity);
        if (auto value = port.write(
                "value",
                lux::ecs::EComponentWireType::SIGNED_INTEGER,
                std::as_bytes(std::span{&link.value, 1})); !value)
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
                std::memcpy(&value.value, property.bytes.data(), sizeof(int));
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
    void appendPod(std::vector<std::byte>& destination, const T& value)
    {
        const std::size_t offset = destination.size();
        destination.resize(offset + sizeof(T));
        std::memcpy(destination.data() + offset, &value, sizeof(T));
    }
}

int main()
{
    const auto link_schema = lux::ecs::makeComponentSchema<Link>(
        lux::ecs::componentSchemaId("test.link"),
        1,
        lux::ecs::ComponentSnapshotMode::Copy,
        lux::ecs::ComponentCodec{&encodeLink, &decodeLink}
    );
    auto schemas = lux::ecs::ComponentSchemaSet::build(
        {lux::ecs::persistentIdComponentSchema(), link_schema}
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
    edit = {};

    const std::array entities{first, second};
    const std::array selected{link_schema.id};
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
    std::uint32_t property_count{};
    std::memcpy(&property_count, future_payload.data(), sizeof(property_count));
    ++property_count;
    std::memcpy(future_payload.data(), &property_count, sizeof(property_count));
    appendPod(future_payload, future_name);
    appendPod(
        future_payload,
        static_cast<std::uint8_t>(lux::ecs::EComponentWireType::BYTES)
    );
    const std::array<std::byte, 3> future_bytes{
        std::byte{1}, std::byte{2}, std::byte{3}};
    appendPod(future_payload, static_cast<std::uint32_t>(future_bytes.size()));
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

    auto loaded = lux::ecs::WorldSectionReader::materialize(*decoded, *schemas);
    assert(loaded);
    auto index = lux::ecs::PersistentEntityIndex::build(**loaded);
    assert(index && index->size() == 2);
    const auto loaded_first = index->find(lux::ecs::PersistentEntityId{uuid("00000000-0000-4000-8000-000000000002")});
    const auto loaded_second = index->find(lux::ecs::PersistentEntityId{uuid("00000000-0000-4000-8000-000000000001")});
    assert((**loaded).get<Link>(loaded_first).value == 7);
    assert((**loaded).get<Link>(loaded_first).target == loaded_second);
    assert((**loaded).get<Link>(loaded_first).external.value == external_id);

    auto truncated = *bytes;
    truncated.pop_back();
    assert(!lux::ecs::decodeWorldSection(truncated));

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
