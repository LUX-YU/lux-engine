#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/scene_format/PersistenceJournal.hpp>

#include <cassert>
#include <span>
#include <type_traits>

int main()
{
    namespace format = lux::ecs::scene_format;

    static_assert(std::is_same_v<
        decltype(format::EntitySectionSchema::id),
        lux::ecs::ComponentSchemaId>);

    format::EntitySectionImage image;
    image.section = format::EntitySectionId{uuids::uuid::from_string(
        "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").value()};
    image.component_names.emplace_back();
    image.schemas.push_back(format::EntitySectionSchema{
        lux::ecs::componentSchemaId("org.lux.test.component"),
        1u,
        format::EEntityComponentStorage::TAG});
    image.archetypes.push_back(format::EntitySectionArchetype{{0u}});
    image.entities.push_back(format::EntitySectionEntity{0u, std::nullopt});

    const auto encoded = format::encodeEntitySectionImage(image);
    assert(encoded);
    const auto decoded = format::decodeEntitySectionImage(*encoded);
    assert(decoded);
    assert(*decoded == image);
    assert(format::entitySectionContentDigest(*encoded) !=
        lux::cxx::algorithm::Sha256Digest{});

    auto invalid = image;
    invalid.schemas.front().id.hash += 1u;
    const auto rejected = format::validateEntitySectionImage(invalid);
    assert(!rejected);
    assert(rejected.error().error ==
        format::EEntitySectionCodecError::INVALID_ARGUMENT);

    const auto base = format::entitySectionContentDigest(*encoded);
    auto journal = format::makePersistenceJournalRecord(
        format::PersistenceSchemaId{"org.lux.test.persistence"},
        1u,
        base,
        1u,
        std::vector<std::byte>{std::byte{0x2a}});
    assert(journal);
    const auto journal_bytes = format::encodePersistenceJournalRecord(*journal);
    assert(journal_bytes);
    const auto journal_roundtrip =
        format::decodePersistenceJournalRecord(*journal_bytes);
    assert(journal_roundtrip && *journal_roundtrip == *journal);
    return 0;
}
