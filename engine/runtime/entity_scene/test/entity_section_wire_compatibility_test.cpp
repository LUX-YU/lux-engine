#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/scene_format/PersistenceJournal.hpp>
#include <lux/engine/resource/entity_scene/EntityPersistenceJournal.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include <cassert>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(std::string_view text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        assert(parsed);
        return *parsed;
    }
}

int main()
{
    namespace legacy = lux::entity_scene;
    namespace format = lux::ecs::scene_format;

    // The compatibility test owns only the frozen wire models. Runtime ECS
    // component contracts are checked by entity_section_public_contract_test,
    // whose target declares the ecs/core dependency explicitly.
    static_assert(!std::is_same_v<
        legacy::PersistentEntityId,
        lux::ecs::PersistentEntityId>);
    static_assert(!std::is_convertible_v<
        legacy::PersistentEntityId,
        lux::ecs::PersistentEntityId>);

    const auto section_uuid =
        uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");

    const auto persistent_uuid =
        uuid("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
    const legacy::PersistentEntityId legacy_persistent{persistent_uuid};
    const lux::ecs::PersistentEntityId runtime_persistent{persistent_uuid};
    assert(legacy_persistent.value() == runtime_persistent.value());

    legacy::EntitySectionImage legacy_image;
    legacy_image.section = legacy::EntitySectionId{section_uuid};
    legacy_image.component_names.emplace_back();
    legacy_image.schemas.push_back(legacy::EntitySectionSchema{
        legacy::ComponentSchemaId{"org.lux.test.component"},
        1u,
        legacy::EEntityComponentStorage::TAG});
    legacy_image.archetypes.push_back(
        legacy::EntitySectionArchetype{{0u}});
    legacy_image.entities.push_back(
        legacy::EntitySectionEntity{0u, std::nullopt});

    format::EntitySectionImage format_image;
    format_image.section = format::EntitySectionId{section_uuid};
    format_image.component_names.emplace_back();
    format_image.schemas.push_back(format::EntitySectionSchema{
        lux::ecs::componentSchemaId("org.lux.test.component"),
        1u,
        format::EEntityComponentStorage::TAG});
    format_image.archetypes.push_back(
        format::EntitySectionArchetype{{0u}});
    format_image.entities.push_back(
        format::EntitySectionEntity{0u, std::nullopt});

    const auto legacy_bytes = legacy::encodeEntitySectionImage(legacy_image);
    const auto format_bytes = format::encodeEntitySectionImage(format_image);
    assert(legacy_bytes && format_bytes);
    assert(*legacy_bytes == *format_bytes);
    assert(legacy::entitySceneContentDigest(*legacy_bytes) ==
        format::entitySectionContentDigest(*format_bytes));

    const auto legacy_roundtrip =
        legacy::decodeEntitySectionImage(*format_bytes);
    const auto format_roundtrip =
        format::decodeEntitySectionImage(*legacy_bytes);
    assert(legacy_roundtrip && *legacy_roundtrip == legacy_image);
    assert(format_roundtrip && *format_roundtrip == format_image);

    const auto base_digest =
        format::entitySectionContentDigest(*format_bytes);
    const std::vector<std::byte> payload{std::byte{0x2a}};

    const auto legacy_record = legacy::makePersistenceJournalRecord(
        legacy::PersistenceSchemaId{"org.lux.test.persistence"},
        1u,
        base_digest,
        1u,
        payload);
    const auto format_record = format::makePersistenceJournalRecord(
        format::PersistenceSchemaId{"org.lux.test.persistence"},
        1u,
        base_digest,
        1u,
        payload);
    assert(legacy_record && format_record);

    const auto legacy_journal =
        legacy::encodePersistenceJournalRecord(*legacy_record);
    const auto format_journal =
        format::encodePersistenceJournalRecord(*format_record);
    assert(legacy_journal && format_journal);
    assert(*legacy_journal == *format_journal);
    assert(legacy::decodePersistenceJournalRecord(*format_journal));
    assert(format::decodePersistenceJournalRecord(*legacy_journal));
    return 0;
}
