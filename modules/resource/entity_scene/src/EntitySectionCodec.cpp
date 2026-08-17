#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include "EntitySceneCodecCommon.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace lux::entity_scene
{
    namespace
    {
        using namespace detail;

        [[nodiscard]] WireNameTable sectionNames(
            const EntitySectionImage& image)
        {
            WireNameTable names;
            for (const auto& schema : image.schemas)
                names.add(schema.id.name());
            for (const auto& attachment : image.attachments)
                names.add(attachment.reference.type.name());
            names.canonicalize();
            return names;
        }

        [[nodiscard]] EntitySceneCodecFailure decodeFailure(
            const std::string& error)
        {
            return failure(readerError(error), error);
        }
    }

    EntitySceneCodecExp<std::vector<std::byte>>
    encodeEntitySectionImage(
        const EntitySectionImage& image,
        const EntitySceneCodecLimits& limits) noexcept
    {
        const auto valid = validateEntitySectionImage(image, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
            const auto names = sectionNames(image);
            if (names.size() > limits.maximum_names)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "LXES structural name table exceeds codec limit"));
            }
            ByteWriter writer;
            writer.reserve(std::min<std::uint64_t>(
                limits.maximum_section_bytes, 256u * 1024u));
            writer.u32(kEntitySectionImageMagic);
            writer.u32(kEntitySectionImageVersion);
            names.write(writer);
            writeUuid(writer, image.section);

            writer.u32(static_cast<std::uint32_t>(
                image.component_names.size()));
            for (std::size_t index = 1u;
                 index < image.component_names.size();
                 ++index)
            {
                writer.str(image.component_names[index]);
            }

            writer.u32(static_cast<std::uint32_t>(image.schemas.size()));
            for (const auto& schema : image.schemas)
            {
                writeStableId(writer, names, schema.id);
                writer.u32(schema.schema_version);
                writer.u8(static_cast<std::uint8_t>(schema.storage));
            }

            writer.u32(static_cast<std::uint32_t>(image.archetypes.size()));
            for (const auto& archetype : image.archetypes)
            {
                writer.u32(static_cast<std::uint32_t>(
                    archetype.schemas.size()));
                for (const auto schema : archetype.schemas)
                    writer.u32(schema);
            }

            writer.u32(static_cast<std::uint32_t>(image.entities.size()));
            for (const auto& entity : image.entities)
            {
                writer.u32(entity.archetype);
                writer.u8(entity.persistent_id ? 1u : 0u);
                if (entity.persistent_id)
                    writeUuid(writer, *entity.persistent_id);
            }

            writer.u32(static_cast<std::uint32_t>(image.columns.size()));
            for (const auto& column : image.columns)
            {
                writer.u32(column.archetype);
                writer.u32(column.schema);
                writer.u32(static_cast<std::uint32_t>(column.offsets.size()));
                for (const auto offset : column.offsets)
                    writer.u32(offset);
                writeBlob(writer, column.payload);
            }

            writer.u32(static_cast<std::uint32_t>(image.parents.size()));
            for (const auto& relation : image.parents)
            {
                writer.u32(relation.child);
                writer.u32(relation.parent);
            }

            writer.u32(static_cast<std::uint32_t>(image.relocations.size()));
            for (const auto& relocation : image.relocations)
            {
                writer.u32(relocation.column);
                writer.u32(relocation.value_index);
                writer.u32(relocation.property_path);
                writer.u32(relocation.target);
            }

            writer.u32(static_cast<std::uint32_t>(
                image.persistent_reference_relocations.size()));
            for (const auto& relocation :
                 image.persistent_reference_relocations)
            {
                writer.u32(relocation.column);
                writer.u32(relocation.value_index);
                writer.u32(relocation.property_path);
                writeUuid(writer, relocation.target);
            }

            writer.u32(static_cast<std::uint32_t>(image.attachments.size()));
            for (const auto& attachment : image.attachments)
            {
                writeDigest(writer, attachment.reference.id.digest);
                writeStableId(writer, names, attachment.reference.type);
                writer.u32(attachment.reference.schema_version);
                writeBlob(writer, attachment.payload);
            }
            writer.u32(static_cast<std::uint32_t>(
                image.blob_relocations.size()));
            for (const auto& relocation : image.blob_relocations)
            {
                writer.u32(relocation.column);
                writer.u32(relocation.value_index);
                writer.u32(relocation.property_path);
                writer.u32(relocation.attachment_index);
            }
            if (writer.size() > limits.maximum_section_bytes)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "encoded LXES exceeds codec limit"));
            }
            return std::move(writer).take();
    }
    EntitySceneCodecExp<EntitySectionImage>
    decodeEntitySectionImage(
        std::span<const std::byte> bytes,
        const EntitySceneCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_section_bytes)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCodecError::LIMIT_EXCEEDED,
                "LXES exceeds codec limit"));
        }
            std::string error;
            ByteReader reader{bytes, &error};
            DecodeAllocationBudget budget{
                bytes.size(), limits.maximum_decode_allocation_bytes};
            std::uint32_t magic = 0u;
            std::uint32_t version = 0u;
            if (!reader.u32(magic) || !reader.u32(version))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::TRUNCATED,
                    "truncated LXES header"));
            }
            if (magic != kEntitySectionImageMagic)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::BAD_MAGIC,
                    "LXES magic mismatch"));
            }
            if (version != kEntitySectionImageVersion)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::UNSUPPORTED_VERSION,
                    "unsupported LXES version"));
            }
            WireNameTable names;
            if (!names.read(reader, limits, budget))
                return lux::cxx::unexpected(decodeFailure(error));

            EntitySectionImage image;
            if (!readUuid(reader, image.section))
                return lux::cxx::unexpected(decodeFailure(error));

            std::uint32_t count = 0u;
            if (!readCount(
                    reader,
                    limits.maximum_names,
                    count,
                    "component name count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (count == 0u)
            {
                error = "component name table has no index-zero sentinel";
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_NAME,
                    error));
            }
            if (!countFitsRemaining(
                    reader,
                    count - 1u,
                    sizeof(std::uint32_t),
                    "component names cannot fit remaining input") ||
                !reserveVector(
                    reader,
                    budget,
                    image.component_names,
                    count,
                    0u,
                    "component names cannot fit remaining input",
                    "component names exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            image.component_names.emplace_back();
            for (std::uint32_t index = 1u; index < count; ++index)
            {
                std::string value;
                if (!readString(
                        reader,
                        value,
                        limits.maximum_string_bytes,
                        budget))
                    return lux::cxx::unexpected(decodeFailure(error));
                if (value.empty() ||
                    (index > 1u && image.component_names.back() >= value))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_NAME,
                        "component name table is not canonical"));
                }
                image.component_names.push_back(std::move(value));
            }

            if (!readCount(
                    reader,
                    limits.maximum_schemas_per_section,
                    count,
                    "schema count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.schemas,
                    count,
                    17u,
                    "schemas cannot fit remaining input",
                    "schemas exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& schema : image.schemas)
            {
                std::uint8_t storage = 0u;
                if (!readStableId(reader, names, schema.id, budget) ||
                    !reader.u32(schema.schema_version) ||
                    !reader.u8(storage))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
                if (storage > static_cast<std::uint8_t>(
                        EEntityComponentStorage::TAG))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "unsupported component storage kind"));
                }
                schema.storage = static_cast<EEntityComponentStorage>(storage);
            }

            if (!readCount(
                    reader,
                    limits.maximum_archetypes_per_section,
                    count,
                    "archetype count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.archetypes,
                    count,
                    4u,
                    "archetypes cannot fit remaining input",
                    "archetypes exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& archetype : image.archetypes)
            {
                std::uint32_t schema_count = 0u;
                if (!readCount(
                        reader,
                        limits.maximum_schemas_per_section,
                        schema_count,
                        "archetype schema count exceeds codec limit"))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
                if (!prepareVector(
                        reader,
                        budget,
                        archetype.schemas,
                        schema_count,
                        4u,
                        "archetype schemas cannot fit remaining input",
                        "archetype schemas exceed decode allocation budget"))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
                for (auto& schema : archetype.schemas)
                {
                    if (!reader.u32(schema))
                        return lux::cxx::unexpected(decodeFailure(error));
                }
            }

            if (!readCount(
                    reader,
                    limits.maximum_entities_per_section,
                    count,
                    "entity count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.entities,
                    count,
                    5u,
                    "entities cannot fit remaining input",
                    "entities exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& entity : image.entities)
            {
                std::uint8_t has_persistent_id = 0u;
                if (!reader.u32(entity.archetype) ||
                    !reader.u8(has_persistent_id))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
                if (has_persistent_id > 1u)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "invalid persistent entity marker"));
                }
                if (has_persistent_id != 0u)
                {
                    PersistentEntityId id;
                    if (!readUuid(reader, id))
                        return lux::cxx::unexpected(decodeFailure(error));
                    entity.persistent_id = id;
                }
            }

            if (!readCount(
                    reader,
                    limits.maximum_columns_per_section,
                    count,
                    "column count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.columns,
                    count,
                    16u,
                    "columns cannot fit remaining input",
                    "columns exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& column : image.columns)
            {
                std::uint32_t offset_count = 0u;
                if (!reader.u32(column.archetype) ||
                    !reader.u32(column.schema) ||
                    !readCount(
                        reader,
                    limits.maximum_entities_per_section ==
                            std::numeric_limits<std::uint32_t>::max()
                        ? limits.maximum_entities_per_section
                        : limits.maximum_entities_per_section + 1u,
                        offset_count,
                        "column offset count exceeds codec limit"))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
                if (!prepareVector(
                        reader,
                        budget,
                        column.offsets,
                        offset_count,
                        4u,
                        "column offsets cannot fit remaining input",
                        "column offsets exceed decode allocation budget"))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
                for (auto& offset : column.offsets)
                {
                    if (!reader.u32(offset))
                        return lux::cxx::unexpected(decodeFailure(error));
                }
                if (!readBlob(
                        reader,
                        column.payload,
                        limits.maximum_section_bytes,
                        budget))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
            }

            if (!readCount(
                    reader,
                    limits.maximum_entities_per_section,
                    count,
                    "parent count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.parents,
                    count,
                    8u,
                    "parents cannot fit remaining input",
                    "parents exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& relation : image.parents)
            {
                if (!reader.u32(relation.child) ||
                    !reader.u32(relation.parent))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
            }

            if (!readCount(
                    reader,
                    limits.maximum_relocations_per_section,
                    count,
                    "relocation count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.relocations,
                    count,
                    16u,
                    "relocations cannot fit remaining input",
                    "relocations exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& relocation : image.relocations)
            {
                if (!reader.u32(relocation.column) ||
                    !reader.u32(relocation.value_index) ||
                    !reader.u32(relocation.property_path) ||
                    !reader.u32(relocation.target))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
            }

            if (!readCount(
                    reader,
                    limits.maximum_relocations_per_section,
                    count,
                    "persistent reference relocation count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.persistent_reference_relocations,
                    count,
                    28u,
                    "persistent reference relocations cannot fit remaining input",
                    "persistent reference relocations exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& relocation : image.persistent_reference_relocations)
            {
                if (!reader.u32(relocation.column) ||
                    !reader.u32(relocation.value_index) ||
                    !reader.u32(relocation.property_path) ||
                    !readUuid(reader, relocation.target))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
            }

            if (!readCount(
                    reader,
                    limits.maximum_attachments_per_section,
                    count,
                    "attachment count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.attachments,
                    count,
                    52u,
                    "attachments cannot fit remaining input",
                    "attachments exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& attachment : image.attachments)
            {
                if (!readDigest(reader, attachment.reference.id.digest) ||
                    !readStableId(
                        reader,
                        names,
                        attachment.reference.type,
                        budget) ||
                    !reader.u32(attachment.reference.schema_version) ||
                    !readBlob(
                        reader,
                        attachment.payload,
                        limits.maximum_section_bytes,
                        budget))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
            }
            if (!readCount(
                    reader,
                    limits.maximum_blob_relocations_per_section,
                    count,
                    "blob relocation count exceeds codec limit"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    image.blob_relocations,
                    count,
                    16u,
                    "blob relocations cannot fit remaining input",
                    "blob relocations exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(decodeFailure(error));
            }
            for (auto& relocation : image.blob_relocations)
            {
                if (!reader.u32(relocation.column) ||
                    !reader.u32(relocation.value_index) ||
                    !reader.u32(relocation.property_path) ||
                    !reader.u32(relocation.attachment_index))
                {
                    return lux::cxx::unexpected(decodeFailure(error));
                }
            }
            if (reader.remaining() != 0u)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::TRAILING_BYTES,
                    "LXES contains trailing bytes"));
            }
            const auto valid = validateEntitySectionImage(image, limits);
            if (!valid)
                return lux::cxx::unexpected(valid.error());
            return image;
    }
}
