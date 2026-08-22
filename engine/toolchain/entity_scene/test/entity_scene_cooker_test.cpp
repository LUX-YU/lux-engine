#include <lux/engine/toolchain/entity_scene/EntitySceneCooker.hpp>
#include <lux/engine/toolchain/entity_scene/EntitySectionImageBuilder.hpp>
#include <lux/engine/toolchain/entity_scene/TaggedPayloadTranscoder.hpp>

#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    struct Field final
    {
        std::uint32_t name{0u};
        lux::ecs::serialization::EArchiveType type{
            lux::ecs::serialization::EArchiveType::UInt32};
        std::vector<std::byte> payload;
    };

    template <class T>
    std::vector<std::byte> pod(T value)
    {
        std::vector<std::byte> bytes;
        lux::serialize::ArchiveWriter writer{bytes};
        writer.writePod(value);
        return bytes;
    }

    std::vector<std::byte> tagged(std::span<const Field> fields)
    {
        std::vector<std::byte> bytes;
        lux::serialize::ArchiveWriter writer{bytes};
        for (const auto& field : fields)
        {
            writer.writePod(field.name);
            writer.writePod(static_cast<std::uint8_t>(field.type));
            writer.writePod(static_cast<std::uint32_t>(
                field.payload.size()));
            writer.writeBytes(field.payload.data(), field.payload.size());
        }
        writer.writePod(lux::ecs::serialization::kEndOfObject);
        return bytes;
    }

    std::uint32_t readU32(
        std::span<const std::byte> bytes,
        std::size_t offset)
    {
        assert(offset + 4u <= bytes.size());
        return static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
            (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
            (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
    }

    lux::toolchain::TaggedPayloadSource referencePayload()
    {
        using lux::ecs::serialization::EArchiveType;
        lux::toolchain::TaggedPayloadSource source;
        source.names = {"", "target", "persistent", "blob"};
        const std::vector<Field> fields{
            {1u, EArchiveType::UInt32, pod<std::uint32_t>(0u)},
            {2u, EArchiveType::UInt32, pod<std::uint32_t>(0u)},
            {3u, EArchiveType::UInt32, pod<std::uint32_t>(0u)}};
        source.payload = tagged(fields);
        return source;
    }

    lux::ecs::scene_format::EntitySectionImage emptyImage(
        const char* id)
    {
        lux::toolchain::EntitySectionImageBuilder builder{
            lux::ecs::scene_format::EntitySectionId{uuid(id)}};
        auto image = std::move(builder).build();
        assert(image);
        return std::move(*image);
    }
}

int main()
{
    using namespace lux::scene;
    using namespace lux::toolchain;

    TaggedPayloadSource nested_source;
    nested_source.names = {"", "zeta", "alpha", "inner"};
    const std::vector<Field> nested_fields{
        {3u,
         lux::ecs::serialization::EArchiveType::UInt32,
         pod<std::uint32_t>(7u)}};
    const auto nested = tagged(nested_fields);
    const std::vector<Field> outer_fields{
        {1u,
         lux::ecs::serialization::EArchiveType::UInt32,
         pod<std::uint32_t>(9u)},
        {2u, lux::ecs::serialization::EArchiveType::Struct, nested}};
    nested_source.payload = tagged(outer_fields);

    const auto canonical_names = canonicalTaggedPayloadNames(
        std::span<const TaggedPayloadSource>{&nested_source, 1u});
    assert(canonical_names);
    assert((*canonical_names ==
        std::vector<std::string>{"", "alpha", "inner", "zeta"}));
    const auto transcoded = transcodeTaggedPayloadNames(
        nested_source, *canonical_names);
    assert(transcoded);
    assert(transcoded->size() == nested_source.payload.size());
    assert(readU32(*transcoded, 0u) == 3u); // zeta
    assert(readU32(*transcoded, 13u) == 1u); // alpha
    assert(readU32(*transcoded, 22u) == 2u); // nested inner

    lux::serialize::NameTable serialized_names;
    assert(serialized_names.intern("second") == 1u);
    assert(serialized_names.intern("first") == 2u);
    std::vector<std::byte> name_table_image;
    lux::serialize::ArchiveWriter name_writer{name_table_image};
    serialized_names.serialize(name_writer);
    const auto decoded_names = decodeTaggedPayloadNameTable(name_table_image);
    assert(decoded_names);
    assert((*decoded_names ==
        std::vector<std::string>{"", "second", "first"}));

    auto malformed = nested_source;
    malformed.payload.pop_back();
    assert(!canonicalTaggedPayloadNames(
        std::span<const TaggedPayloadSource>{&malformed, 1u}));

    const lux::ecs::scene_format::EntitySectionId section_id{
        uuid("20000000-0000-4000-8000-000000000001")};
    EntitySectionImageBuilder builder{section_id};
    const auto z_attachment = builder.addAttachment({
        lux::ecs::scene_format::ContentTypeId{"lux.test.z_blob"},
        1u,
        pod<std::uint32_t>(11u)});
    const auto a_attachment = builder.addAttachment({
        lux::ecs::scene_format::ContentTypeId{"lux.test.a_blob"},
        1u,
        pod<std::uint32_t>(22u)});
    assert(z_attachment && *z_attachment == 0u);
    assert(a_attachment && *a_attachment == 1u);

    EntityCookInput parent;
    parent.persistent_id = lux::ecs::PersistentEntityId{
        uuid("30000000-0000-4000-8000-000000000001")};
    EntityComponentCookInput parent_tag;
    parent_tag.schema = lux::ecs::componentSchemaId("lux.test.b_tag");
    parent_tag.storage =
        lux::ecs::scene_format::EEntityComponentStorage::TAG;
    parent.components.push_back(std::move(parent_tag));
    const auto parent_ordinal = builder.addEntity(std::move(parent));
    assert(parent_ordinal && *parent_ordinal == 0u);

    EntityCookInput child;
    child.parent = *parent_ordinal;
    EntityComponentCookInput data;
    data.schema = lux::ecs::componentSchemaId("lux.test.a_data");
    data.value = referencePayload();
    data.local_references.push_back({"target", *parent_ordinal});
    data.persistent_references.push_back({
        "persistent",
        lux::ecs::PersistentEntityId{
            uuid("30000000-0000-4000-8000-000000000099")}});
    data.blob_references.push_back({"blob", *z_attachment});
    child.components.push_back(std::move(data));
    EntityComponentCookInput child_tag;
    child_tag.schema = lux::ecs::componentSchemaId("lux.test.b_tag");
    child_tag.storage =
        lux::ecs::scene_format::EEntityComponentStorage::TAG;
    child.components.push_back(std::move(child_tag));
    const auto child_ordinal = builder.addEntity(std::move(child));
    assert(child_ordinal && *child_ordinal == 1u);

    auto image = std::move(builder).build();
    assert(image);
    assert(lux::ecs::scene_format::validateEntitySectionImage(*image));
    assert(image->schemas.size() == 2u);
    assert(image->schemas[0u].id.name == "lux.test.a_data");
    assert(image->schemas[1u].id.name == "lux.test.b_tag");
    assert(image->archetypes.size() == 2u);
    assert((image->archetypes[0u].schemas ==
        std::vector<std::uint32_t>{0u, 1u}));
    assert((image->archetypes[1u].schemas ==
        std::vector<std::uint32_t>{1u}));
    assert(image->parents.size() == 1u);
    assert(image->parents[0u].child == 0u);
    assert(image->parents[0u].parent == 1u);
    assert(image->relocations.size() == 1u);
    assert(image->relocations[0u].target == 1u);
    assert(image->blob_relocations.size() == 1u);
    assert(image->blob_relocations[0u].attachment_index == 1u);
    assert(image->attachments[0u].reference.type.name() ==
        "lux.test.a_blob");
    assert(image->attachments[1u].reference.type.name() ==
        "lux.test.z_blob");

    SceneDescriptionCookInput scene;
    scene.id = lux::asset::asset_id_t{
        uuid("10000000-0000-4000-8000-000000000001")};
    scene.features.push_back({
        SceneFeatureId{"lux.test.presentation"},
        1u,
        {}});
    scene.startup_sections.push_back(
        lux::ecs::scene_format::EntitySectionId{section_id.value()});

    EntitySectionCookInput later;
    later.image = emptyImage(
        "20000000-0000-4000-8000-000000000002");
    later.source = lux::scene::StoredSectionSource{
        "/Game/EntitySections/20000000_0000_4000_8000_000000000002"};
    later.dependencies.push_back(
        lux::ecs::scene_format::EntitySectionId{section_id.value()});
    scene.sections.push_back(std::move(later));

    EntitySectionCookInput startup;
    startup.image = std::move(*image);
    startup.source = lux::scene::StoredSectionSource{
        "/Game/EntitySections/20000000_0000_4000_8000_000000000001"};
    startup.demand_channels.push_back(
        lux::scene::DemandChannelId{"lux.test.startup"});
    scene.sections.push_back(std::move(startup));

    auto cooked = cookSceneDescription(std::move(scene));
    assert(cooked);
    assert(validateSceneDescription(cooked->package));
    assert(cooked->sections.size() == 2u);
    assert(cooked->sections[0u].record.id ==
        lux::ecs::scene_format::EntitySectionId{section_id.value()});
    assert(cooked->package.sections[0u] ==
        cooked->sections[0u].record);
    assert(cooked->sections[0u].record.content_digest ==
        lux::ecs::scene_format::entitySectionContentDigest(
            cooked->sections[0u].encoded_image));
    assert(cooked->sections[0u].record.required_components.size() == 2u);
    assert(cooked->package.required_components.size() == 2u);
    const auto decoded_package = SceneAssetSerDeser::decodeData(
        cooked->encoded_package);
    assert(decoded_package && **decoded_package == cooked->package);

    return 0;
}
