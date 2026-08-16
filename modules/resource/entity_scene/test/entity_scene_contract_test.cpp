#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <span>
#include <utility>
#include <vector>

namespace
{
    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    std::vector<std::byte> bytes(
        std::initializer_list<std::uint8_t> values)
    {
        std::vector<std::byte> result;
        result.reserve(values.size());
        for (const auto value : values)
            result.push_back(static_cast<std::byte>(value));
        return result;
    }

    std::uint32_t readU32(
        std::span<const std::byte> image,
        std::size_t offset)
    {
        assert(offset + 4u <= image.size());
        return static_cast<std::uint32_t>(image[offset]) |
            (static_cast<std::uint32_t>(image[offset + 1u]) << 8u) |
            (static_cast<std::uint32_t>(image[offset + 2u]) << 16u) |
            (static_cast<std::uint32_t>(image[offset + 3u]) << 24u);
    }

    void writeU32(
        std::span<std::byte> image,
        std::size_t offset,
        std::uint32_t value)
    {
        assert(offset + 4u <= image.size());
        for (std::size_t byte = 0u; byte < 4u; ++byte)
        {
            image[offset + byte] = static_cast<std::byte>(
                (value >> (byte * 8u)) & 0xffu);
        }
    }

    std::size_t componentNameCountOffset(
        std::span<const std::byte> encoded_section)
    {
        std::size_t offset = 8u;
        const auto structural_name_count = readU32(encoded_section, offset);
        offset += 4u;
        for (std::uint32_t index = 1u;
             index < structural_name_count;
             ++index)
        {
            const auto length = readU32(encoded_section, offset);
            offset += 4u + length;
        }
        return offset + 16u;
    }

    std::size_t firstSchemaHashOffset(
        std::span<const std::byte> encoded_section)
    {
        auto offset = componentNameCountOffset(encoded_section);
        const auto count = readU32(encoded_section, offset);
        offset += 4u;
        for (std::uint32_t index = 1u; index < count; ++index)
        {
            const auto length = readU32(encoded_section, offset);
            offset += 4u + length;
        }
        assert(readU32(encoded_section, offset) != 0u);
        return offset + 4u;
    }

    lux::entity_scene::EntitySectionImage sectionImage()
    {
        using namespace lux::entity_scene;
        EntitySectionImage image;
        image.section = EntitySectionId{
            uuid("20000000-0000-4000-8000-000000000001")};
        image.component_names = {
            "", "content", "position", "rotation", "scale"};
        image.schemas = {
            {ComponentSchemaId{"org.lux.active"}, 1u,
             EEntityComponentStorage::TAG},
            // Equal-length owning StableId names guard duplicate detection
            // against retaining string_views to projection temporaries.
            {ComponentSchemaId{"org.lux.marker_a"}, 1u,
             EEntityComponentStorage::TAG},
            {ComponentSchemaId{"org.lux.marker_b"}, 1u,
             EEntityComponentStorage::TAG},
            {ComponentSchemaId{"org.lux.transform3d"}, 1u,
             EEntityComponentStorage::DATA}};
        image.archetypes.push_back({{0u, 1u, 2u, 3u}});
        image.entities = {
            {0u, PersistentEntityId{
                uuid("30000000-0000-4000-8000-000000000001")}},
            {0u, std::nullopt}};
        image.columns.push_back({
            0u,
            3u,
            {0u, 8u, 16u},
            bytes({0u, 0u, 0u, 0u, 1u, 2u, 3u, 4u,
                   0u, 0u, 0u, 0u, 5u, 6u, 7u, 8u})});
        image.parents.push_back({1u, 0u});
        image.relocations.push_back({0u, 1u, 2u, 0u});
        image.persistent_reference_relocations.push_back({
            0u,
            0u,
            3u,
            PersistentEntityId{
                uuid("30000000-0000-4000-8000-000000000099")}});
        EntitySectionAttachment attachment;
        attachment.reference.type = ContentTypeId{
            "org.lux.classic_mesh_batch"};
        attachment.reference.schema_version = 1u;
        attachment.payload = bytes({9u, 8u, 7u, 6u});
        attachment.reference.id = makeContentBlobId(
            attachment.reference.type,
            attachment.reference.schema_version,
            attachment.payload);
        image.attachments.push_back(std::move(attachment));
        image.blob_relocations.push_back({0u, 0u, 1u, 0u});
        return image;
    }

    lux::entity_scene::EntitySectionImage emptySectionImage()
    {
        using namespace lux::entity_scene;
        EntitySectionImage image;
        image.section = EntitySectionId{
            uuid("21000000-0000-4000-8000-000000000001")};
        image.component_names = {""};
        return image;
    }

    lux::entity_scene::EntitySceneManifest emptySceneManifest()
    {
        using namespace lux::entity_scene;
        EntitySceneManifest manifest;
        manifest.id = EntitySceneId{
            uuid("11000000-0000-4000-8000-000000000001")};
        return manifest;
    }

    lux::entity_scene::EntitySceneManifest sceneManifest(
        std::span<const std::byte> section_bytes)
    {
        using namespace lux::entity_scene;
        EntitySceneManifest manifest;
        manifest.id = EntitySceneId{
            uuid("10000000-0000-4000-8000-000000000001")};
        manifest.contributions.push_back({
            lux::extensions::ContributionId{"org.lux.presentation3d"},
            1u,
            bytes({1u, 2u})});
        EntitySectionRecord startup;
        startup.id = EntitySectionId{
            uuid("20000000-0000-4000-8000-000000000001")};
        startup.source = StoredSectionSource{
            "/Game/Scenes/Test/startup_lxes"};
        startup.content_digest = entitySceneContentDigest(section_bytes);
        startup.encoded_bytes = section_bytes.size();
        startup.decoded_bytes = section_bytes.size();
        startup.entity_count = 2u;
        startup.demand_channels.push_back(
            DemandChannelId{"org.lux.startup"});
        startup.required_components.push_back({
            ComponentSchemaId{"org.lux.transform3d"}, 1u});
        manifest.startup_sections.push_back(startup.id);
        manifest.sections.push_back(startup);

        EntitySectionRecord generated;
        generated.id = EntitySectionId{
            uuid("20000000-0000-4000-8000-000000000002")};
        generated.source = GeneratedSectionSource{
            SectionGeneratorId{"org.lux.test.generator"},
            42u,
            bytes({4u, 5u, 6u})};
        generated.content_digest = entitySceneContentDigest(section_bytes);
        generated.encoded_bytes = section_bytes.size();
        generated.decoded_bytes = section_bytes.size();
        generated.entity_count = 2u;
        generated.dependencies.push_back(startup.id);
        generated.demand_channels.push_back(
            DemandChannelId{"org.lux.spatial2d"});
        manifest.sections.push_back(std::move(generated));
        manifest.required_extensions.push_back({
            lux::extensions::ExtensionId{"org.lux.test"}, 1u, 0u});
        return manifest;
    }
}

int main()
{
    using namespace lux::entity_scene;

    auto section = sectionImage();
    assert(validateEntitySectionImage(section));
    const auto encoded_section = encodeEntitySectionImage(section);
    assert(encoded_section);
    const auto encoded_section_again = encodeEntitySectionImage(section);
    assert(encoded_section_again);
    assert(*encoded_section == *encoded_section_again);
    const auto decoded_section = decodeEntitySectionImage(*encoded_section);
    assert(decoded_section);
    assert(*decoded_section == section);
    const auto reencoded_section = encodeEntitySectionImage(*decoded_section);
    assert(reencoded_section);
    assert(*reencoded_section == *encoded_section);

    auto manifest = sceneManifest(*encoded_section);
    for (const auto& record : manifest.sections)
        assert(validateEntitySectionRecord(record));
    assert(validateEntitySceneManifest(manifest));
    const auto encoded_manifest = encodeEntitySceneManifest(manifest);
    assert(encoded_manifest);
    const auto encoded_manifest_again = encodeEntitySceneManifest(manifest);
    assert(encoded_manifest_again);
    assert(*encoded_manifest == *encoded_manifest_again);
    const auto decoded_manifest = decodeEntitySceneManifest(*encoded_manifest);
    assert(decoded_manifest);
    assert(*decoded_manifest == manifest);
    const auto reencoded_manifest = encodeEntitySceneManifest(*decoded_manifest);
    assert(reencoded_manifest);
    assert(*reencoded_manifest == *encoded_manifest);

    auto wrong_magic = *encoded_section;
    wrong_magic.front() ^= std::byte{0xffu};
    const auto bad_magic = decodeEntitySectionImage(wrong_magic);
    assert(!bad_magic);
    assert(bad_magic.error().error == EEntitySceneCodecError::BAD_MAGIC);

    auto wrong_version = *encoded_manifest;
    wrong_version[4u] = std::byte{2u};
    const auto bad_version = decodeEntitySceneManifest(wrong_version);
    assert(!bad_version);
    assert(bad_version.error().error ==
        EEntitySceneCodecError::UNSUPPORTED_VERSION);

    auto trailing = *encoded_manifest;
    trailing.push_back(std::byte{0u});
    const auto bad_trailing = decodeEntitySceneManifest(trailing);
    assert(!bad_trailing);
    assert(bad_trailing.error().error ==
        EEntitySceneCodecError::TRAILING_BYTES);

    const auto truncated = decodeEntitySectionImage(
        std::span<const std::byte>{
            encoded_section->data(), encoded_section->size() - 1u});
    assert(!truncated);

    auto malformed_component_names = *encoded_section;
    writeU32(
        malformed_component_names,
        componentNameCountOffset(malformed_component_names),
        0u);
    const auto malformed_names =
        decodeEntitySectionImage(malformed_component_names);
    assert(!malformed_names);
    assert(malformed_names.error().error ==
        EEntitySceneCodecError::INVALID_NAME);

    auto mismatched_schema_identity = *encoded_section;
    mismatched_schema_identity[firstSchemaHashOffset(
        mismatched_schema_identity)] ^= std::byte{1u};
    const auto bad_schema_identity =
        decodeEntitySectionImage(mismatched_schema_identity);
    assert(!bad_schema_identity);
    assert(bad_schema_identity.error().error ==
        EEntitySceneCodecError::HASH_MISMATCH);

    // Collection counts are rejected from the bytes still available before
    // the decoder reserves or resizes a destination container.
    const auto encoded_empty_section =
        encodeEntitySectionImage(emptySectionImage());
    assert(encoded_empty_section);
    auto short_structural_names = std::vector<std::byte>(
        encoded_empty_section->begin(), encoded_empty_section->begin() + 12u);
    writeU32(short_structural_names, 8u, 1u << 20u);
    const auto bad_structural_names =
        decodeEntitySectionImage(short_structural_names);
    assert(!bad_structural_names);
    assert(bad_structural_names.error().error ==
        EEntitySceneCodecError::TRUNCATED);

    auto short_schemas = *encoded_empty_section;
    short_schemas.resize(36u);
    writeU32(short_schemas, 32u, 65536u);
    const auto bad_schema_count = decodeEntitySectionImage(short_schemas);
    assert(!bad_schema_count);
    assert(bad_schema_count.error().error ==
        EEntitySceneCodecError::TRUNCATED);

    const auto encoded_empty_manifest =
        encodeEntitySceneManifest(emptySceneManifest());
    assert(encoded_empty_manifest);
    auto short_contributions = *encoded_empty_manifest;
    short_contributions.resize(32u);
    writeU32(short_contributions, 28u, 65536u);
    const auto bad_contribution_count =
        decodeEntitySceneManifest(short_contributions);
    assert(!bad_contribution_count);
    assert(bad_contribution_count.error().error ==
        EEntitySceneCodecError::TRUNCATED);

    EntitySceneCodecLimits no_decode_allocations;
    no_decode_allocations.maximum_decode_allocation_bytes = 1u;
    const auto allocation_limited = decodeEntitySectionImage(
        *encoded_empty_section, no_decode_allocations);
    assert(!allocation_limited);
    assert(allocation_limited.error().error ==
        EEntitySceneCodecError::LIMIT_EXCEEDED);

    auto duplicate_manifest = manifest;
    duplicate_manifest.sections.push_back(
        duplicate_manifest.sections.front());
    const auto duplicate = validateEntitySceneManifest(duplicate_manifest);
    assert(!duplicate);
    assert(duplicate.error().error == EEntitySceneCodecError::DUPLICATE_ID);

    auto missing_startup = manifest;
    missing_startup.startup_sections.front() = EntitySectionId{
        uuid("20000000-0000-4000-8000-000000000099")};
    const auto missing = validateEntitySceneManifest(missing_startup);
    assert(!missing);
    assert(missing.error().error ==
        EEntitySceneCodecError::INVALID_REFERENCE);

    auto oversized_record = manifest;
    oversized_record.sections.front().encoded_bytes =
        EntitySceneCodecLimits{}.maximum_section_bytes + 1u;
    const auto oversized_manifest =
        validateEntitySceneManifest(oversized_record);
    assert(!oversized_manifest);
    assert(oversized_manifest.error().error ==
        EEntitySceneCodecError::INVALID_ARGUMENT);

    oversized_record = manifest;
    oversized_record.sections.front().decoded_bytes =
        EntitySceneCodecLimits{}.maximum_section_bytes + 1u;
    assert(!validateEntitySceneManifest(oversized_record));

    auto mismatched_uncompressed_size = manifest;
    ++mismatched_uncompressed_size.sections.front().decoded_bytes;
    const auto mismatched_size =
        validateEntitySceneManifest(mismatched_uncompressed_size);
    assert(!mismatched_size);
    assert(mismatched_size.error().error ==
        EEntitySceneCodecError::INVALID_ARGUMENT);

    auto compressed_size = mismatched_uncompressed_size;
    compressed_size.sections.front().compression =
        EEntitySectionCompression::ZSTD;
    assert(validateEntitySceneManifest(compressed_size));

    auto compressed_generated = manifest.sections.back();
    compressed_generated.compression = EEntitySectionCompression::ZSTD;
    const auto invalid_generated_record =
        validateEntitySectionRecord(compressed_generated);
    assert(!invalid_generated_record);
    assert(invalid_generated_record.error().error ==
        EEntitySceneCodecError::INVALID_ARGUMENT);

    for (const std::string invalid_path : {
             "Game/Scenes/Test/startup_lxes",
             "/Game\\Scenes\\Test\\startup_lxes",
             "/Game/Scenes/./startup_lxes",
             "/Game/Scenes/../startup_lxes",
             "/Game//startup_lxes",
             "/Game/Scenes/startup_lxes/"})
    {
        auto invalid_stored_path = manifest;
        std::get<StoredSectionSource>(
            invalid_stored_path.sections.front().source).content_path =
                invalid_path;
        assert(!validateEntitySceneManifest(invalid_stored_path));
    }

    auto missing_column = section;
    missing_column.columns.clear();
    assert(!validateEntitySectionImage(missing_column));

    auto missing_name_sentinel = section;
    missing_name_sentinel.component_names.front() = "not-empty";
    const auto missing_sentinel =
        validateEntitySectionImage(missing_name_sentinel);
    assert(!missing_sentinel);
    assert(missing_sentinel.error().error ==
        EEntitySceneCodecError::INVALID_ARGUMENT);

    auto duplicate_component_name = section;
    duplicate_component_name.component_names = {"", "position", "position"};
    const auto duplicate_name =
        validateEntitySectionImage(duplicate_component_name);
    assert(!duplicate_name);
    assert(duplicate_name.error().error ==
        EEntitySceneCodecError::INVALID_NAME);

    auto unsorted_component_names = section;
    unsorted_component_names.component_names = {"", "scale", "position"};
    assert(!validateEntitySectionImage(unsorted_component_names));

    auto oversized_component_name = section;
    oversized_component_name.component_names[1u] =
        std::string(4097u, 'x');
    const auto oversized_name =
        validateEntitySectionImage(oversized_component_name);
    assert(!oversized_name);
    assert(oversized_name.error().error ==
        EEntitySceneCodecError::INVALID_NAME);

    auto cyclic = section;
    cyclic.parents = {{0u, 1u}, {1u, 0u}};
    const auto cycle = validateEntitySectionImage(cyclic);
    assert(!cycle);
    assert(cycle.error().error ==
        EEntitySceneCodecError::INVALID_REFERENCE);

    auto missing_relocation_property = section;
    missing_relocation_property.relocations.front().property_path = 0u;
    const auto missing_property =
        validateEntitySectionImage(missing_relocation_property);
    assert(!missing_property);
    assert(missing_property.error().error ==
        EEntitySceneCodecError::INVALID_REFERENCE);

    auto out_of_range_relocation_property = section;
    out_of_range_relocation_property.relocations.front().property_path = 99u;
    assert(!validateEntitySectionImage(out_of_range_relocation_property));

    auto empty_persistent_reference = section;
    empty_persistent_reference.persistent_reference_relocations.front()
        .target = {};
    assert(!validateEntitySectionImage(empty_persistent_reference));

    auto missing_persistent_reference_property = section;
    missing_persistent_reference_property.persistent_reference_relocations
        .front().property_path = 0u;
    assert(!validateEntitySectionImage(
        missing_persistent_reference_property));

    auto duplicate_persistent_reference = section;
    duplicate_persistent_reference.persistent_reference_relocations.push_back(
        duplicate_persistent_reference.persistent_reference_relocations
            .front());
    assert(!validateEntitySectionImage(duplicate_persistent_reference));

    auto missing_blob_property = section;
    missing_blob_property.blob_relocations.front().property_path = 0u;
    assert(!validateEntitySectionImage(missing_blob_property));

    auto invalid_blob_attachment = section;
    invalid_blob_attachment.blob_relocations.front().attachment_index = 99u;
    assert(!validateEntitySectionImage(invalid_blob_attachment));

    auto digest_mismatch = section;
    digest_mismatch.attachments.front().payload.front() ^= std::byte{1u};
    const auto mismatch = validateEntitySectionImage(digest_mismatch);
    assert(!mismatch);
    assert(mismatch.error().error ==
        EEntitySceneCodecError::DIGEST_MISMATCH);

    EntitySceneCodecLimits tight_limits;
    tight_limits.maximum_entities_per_section = 1u;
    const auto limited = decodeEntitySectionImage(
        *encoded_section, tight_limits);
    assert(!limited);
    assert(limited.error().error == EEntitySceneCodecError::LIMIT_EXCEEDED);

    const ContentTypeId first_type{"org.lux.first"};
    const ContentTypeId second_type{"org.lux.second"};
    const auto payload = bytes({1u, 2u, 3u});
    assert(makeContentBlobId(first_type, 1u, payload) ==
        makeContentBlobId(first_type, 1u, payload));
    assert(makeContentBlobId(first_type, 1u, payload) !=
        makeContentBlobId(first_type, 2u, payload));
    assert(makeContentBlobId(first_type, 1u, payload) !=
        makeContentBlobId(second_type, 1u, payload));
    return 0;
}
