#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include "EntitySceneCodecCommon.hpp"

#include <algorithm>
#include <utility>

namespace lux::entity_scene
{
    namespace
    {
        using namespace detail;

        void addRequirementsToNames(
            WireNameTable& names,
            std::span<const RequiredExtension> extensions,
            std::span<const RequiredComponentSchema> components)
        {
            for (const auto& value : extensions)
                names.add(value.id.name());
            for (const auto& value : components)
                names.add(value.id.name());
        }

        void writeRequirements(
            ByteWriter& writer,
            const WireNameTable& names,
            std::span<const RequiredExtension> extensions,
            std::span<const RequiredComponentSchema> components)
        {
            writer.u32(static_cast<std::uint32_t>(extensions.size()));
            for (const auto& value : extensions)
            {
                writeExtensionId(writer, names, value.id);
                writer.u16(value.required_major);
                writer.u16(value.minimum_minor);
            }
            writer.u32(static_cast<std::uint32_t>(components.size()));
            for (const auto& value : components)
            {
                writeStableId(writer, names, value.id);
                writer.u32(value.schema_version);
            }
        }

        [[nodiscard]] bool readRequirements(
            ByteReader& reader,
            const WireNameTable& names,
            std::vector<RequiredExtension>& extensions,
            std::vector<RequiredComponentSchema>& components,
            const EntitySceneCodecLimits& limits,
            DecodeAllocationBudget& budget) noexcept
        {
            std::uint32_t count = 0u;
            if (!readCount(
                    reader,
                    limits.maximum_requirements,
                    count,
                    "extension requirement count exceeds codec limit"))
            {
                return false;
            }
            if (!prepareVector(
                    reader,
                    budget,
                    extensions,
                    count,
                    16u,
                    "extension requirements cannot fit remaining input",
                    "extension requirements exceed decode allocation budget"))
            {
                return false;
            }
            for (auto& value : extensions)
            {
                if (!readExtensionId(
                        reader, names, value.id, budget) ||
                    !reader.u16(value.required_major) ||
                    !reader.u16(value.minimum_minor))
                {
                    return false;
                }
            }
            if (!readCount(
                    reader,
                    limits.maximum_requirements,
                    count,
                    "component requirement count exceeds codec limit"))
            {
                return false;
            }
            if (!prepareVector(
                    reader,
                    budget,
                    components,
                    count,
                    16u,
                    "component requirements cannot fit remaining input",
                    "component requirements exceed decode allocation budget"))
            {
                return false;
            }
            for (auto& value : components)
            {
                if (!readStableId(reader, names, value.id, budget) ||
                    !reader.u32(value.schema_version))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] WireNameTable manifestNames(
            const EntitySceneManifest& manifest)
        {
            WireNameTable names;
            for (const auto& contribution : manifest.contributions)
                names.add(contribution.id.name());
            addRequirementsToNames(
                names,
                manifest.required_extensions,
                manifest.required_components);
            for (const auto& section : manifest.sections)
            {
                if (const auto* stored =
                        std::get_if<StoredSectionSource>(&section.source))
                {
                    names.add(stored->content_path);
                }
                else
                {
                    names.add(std::get<GeneratedSectionSource>(
                        section.source).generator.name());
                }
                for (const auto& channel : section.demand_channels)
                    names.add(channel.name());
                addRequirementsToNames(
                    names,
                    section.required_extensions,
                    section.required_components);
            }
            names.canonicalize();
            return names;
        }

        void writeSectionRecord(
            ByteWriter& writer,
            const WireNameTable& names,
            const EntitySectionRecord& section)
        {
            writeUuid(writer, section.id);
            if (const auto* stored =
                    std::get_if<StoredSectionSource>(&section.source))
            {
                writer.u8(0u);
                writer.u32(names.index(stored->content_path));
            }
            else
            {
                const auto& generated =
                    std::get<GeneratedSectionSource>(section.source);
                writer.u8(1u);
                writeStableId(writer, names, generated.generator);
                writer.u64(generated.seed);
                writeBlob(writer, generated.parameters);
            }
            writeDigest(writer, section.content_digest);
            writer.u8(static_cast<std::uint8_t>(section.compression));
            writer.u64(section.encoded_bytes);
            writer.u64(section.decoded_bytes);
            writer.u32(section.entity_count);

            writer.u32(static_cast<std::uint32_t>(
                section.dependencies.size()));
            for (const auto& dependency : section.dependencies)
                writeUuid(writer, dependency);

            writer.u32(static_cast<std::uint32_t>(
                section.demand_channels.size()));
            for (const auto& channel : section.demand_channels)
                writeStableId(writer, names, channel);

            writeRequirements(
                writer,
                names,
                section.required_extensions,
                section.required_components);
        }

        [[nodiscard]] bool readSectionRecord(
            ByteReader& reader,
            const WireNameTable& names,
            EntitySectionRecord& section,
            const EntitySceneCodecLimits& limits,
            DecodeAllocationBudget& budget) noexcept
        {
            if (!readUuid(reader, section.id))
                return false;
            std::uint8_t source_kind = 0u;
            if (!reader.u8(source_kind))
                return false;
            if (source_kind == 0u)
            {
                std::uint32_t name_index = 0u;
                if (!reader.u32(name_index))
                    return false;
                const auto path = names.at(name_index);
                if (path.empty())
                {
                    reader.fail("stored Section source references invalid name");
                    return false;
                }
                if (!budget.consume(
                        reader,
                        path.size(),
                        sizeof(char),
                        "stored Section paths exceed decode allocation budget"))
                {
                    return false;
                }
                section.source = StoredSectionSource{std::string{path}};
            }
            else if (source_kind == 1u)
            {
                GeneratedSectionSource generated;
                if (!readStableId(
                        reader, names, generated.generator, budget) ||
                    !reader.u64(generated.seed) ||
                    !readBlob(
                        reader,
                        generated.parameters,
                        limits.maximum_generator_parameter_bytes,
                        budget))
                {
                    return false;
                }
                section.source = std::move(generated);
            }
            else
            {
                reader.fail("unsupported Section source kind");
                return false;
            }

            std::uint8_t compression = 0u;
            if (!readDigest(reader, section.content_digest) ||
                !reader.u8(compression) ||
                !reader.u64(section.encoded_bytes) ||
                !reader.u64(section.decoded_bytes) ||
                !reader.u32(section.entity_count))
            {
                return false;
            }
            if (compression > static_cast<std::uint8_t>(
                    EEntitySectionCompression::ZSTD))
            {
                reader.fail("unsupported Section compression");
                return false;
            }
            section.compression =
                static_cast<EEntitySectionCompression>(compression);

            std::uint32_t count = 0u;
            if (!readCount(
                    reader,
                    limits.maximum_dependencies_per_section,
                    count,
                    "dependency count exceeds codec limit"))
            {
                return false;
            }
            if (!prepareVector(
                    reader,
                    budget,
                    section.dependencies,
                    count,
                    16u,
                    "dependencies cannot fit remaining input",
                    "dependencies exceed decode allocation budget"))
            {
                return false;
            }
            for (auto& dependency : section.dependencies)
            {
                if (!readUuid(reader, dependency))
                    return false;
            }
            if (!readCount(
                    reader,
                    limits.maximum_requirements,
                    count,
                    "demand channel count exceeds codec limit"))
            {
                return false;
            }
            if (!prepareVector(
                    reader,
                    budget,
                    section.demand_channels,
                    count,
                    12u,
                    "demand channels cannot fit remaining input",
                    "demand channels exceed decode allocation budget"))
            {
                return false;
            }
            for (auto& channel : section.demand_channels)
            {
                if (!readStableId(reader, names, channel, budget))
                    return false;
            }
            return readRequirements(
                reader,
                names,
                section.required_extensions,
                section.required_components,
                limits,
                budget);
        }
    }

    lux::cxx::expected<std::vector<std::byte>, EntitySceneCodecFailure>
    encodeEntitySceneManifest(
        const EntitySceneManifest& manifest,
        const EntitySceneCodecLimits& limits) noexcept
    {
        const auto valid = validateEntitySceneManifest(manifest, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
            const auto names = manifestNames(manifest);
            if (names.size() > limits.maximum_names)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "LXSC structural name table exceeds codec limit"));
            }
            ByteWriter writer;
            writer.reserve(std::min<std::uint64_t>(
                limits.maximum_manifest_bytes, 64u * 1024u));
            writer.u32(kEntitySceneManifestMagic);
            writer.u32(kEntitySceneManifestVersion);
            names.write(writer);
            writeUuid(writer, manifest.id);

            writer.u32(static_cast<std::uint32_t>(
                manifest.contributions.size()));
            for (const auto& contribution : manifest.contributions)
            {
                writeContributionId(writer, names, contribution.id);
                writer.u32(contribution.config_schema_version);
                writeBlob(writer, contribution.config);
            }
            writer.u32(static_cast<std::uint32_t>(
                manifest.startup_sections.size()));
            for (const auto& section : manifest.startup_sections)
                writeUuid(writer, section);

            writer.u32(static_cast<std::uint32_t>(manifest.sections.size()));
            for (const auto& section : manifest.sections)
                writeSectionRecord(writer, names, section);

            writeRequirements(
                writer,
                names,
                manifest.required_extensions,
                manifest.required_components);
            if (writer.size() > limits.maximum_manifest_bytes)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "encoded LXSC exceeds codec limit"));
            }
            return std::move(writer).take();
    }
    lux::cxx::expected<EntitySceneManifest, EntitySceneCodecFailure>
    decodeEntitySceneManifest(
        std::span<const std::byte> bytes,
        const EntitySceneCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_manifest_bytes)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCodecError::LIMIT_EXCEEDED,
                "LXSC exceeds codec limit"));
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
                    "truncated LXSC header"));
            }
            if (magic != kEntitySceneManifestMagic)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::BAD_MAGIC,
                    "LXSC magic mismatch"));
            }
            if (version != kEntitySceneManifestVersion)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::UNSUPPORTED_VERSION,
                    "unsupported LXSC version"));
            }
            WireNameTable names;
            if (!names.read(reader, limits, budget))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }

            EntitySceneManifest manifest;
            if (!readUuid(reader, manifest.id))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::TRUNCATED,
                    error));
            }
            std::uint32_t count = 0u;
            if (!readCount(
                    reader,
                    limits.maximum_contributions,
                    count,
                    "contribution count exceeds codec limit"))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    manifest.contributions,
                    count,
                    20u,
                    "contributions cannot fit remaining input",
                    "contributions exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            for (auto& contribution : manifest.contributions)
            {
                if (!readContributionId(
                        reader, names, contribution.id, budget) ||
                    !reader.u32(contribution.config_schema_version) ||
                    !readBlob(
                        reader,
                        contribution.config,
                        limits.maximum_generator_parameter_bytes,
                        budget))
                {
                    return lux::cxx::unexpected(failure(
                        readerError(error), error));
                }
            }
            if (!readCount(
                    reader,
                    limits.maximum_sections,
                    count,
                    "startup Section count exceeds codec limit"))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    manifest.startup_sections,
                    count,
                    16u,
                    "startup Sections cannot fit remaining input",
                    "startup Sections exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            for (auto& section : manifest.startup_sections)
            {
                if (!readUuid(reader, section))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::TRUNCATED,
                        error));
                }
            }
            if (!readCount(
                    reader,
                    limits.maximum_sections,
                    count,
                    "EntitySection record count exceeds codec limit"))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            if (!prepareVector(
                    reader,
                    budget,
                    manifest.sections,
                    count,
                    90u,
                    "EntitySection records cannot fit remaining input",
                    "EntitySection records exceed decode allocation budget"))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            for (auto& section : manifest.sections)
            {
                if (!readSectionRecord(
                        reader, names, section, limits, budget))
                {
                    return lux::cxx::unexpected(failure(
                        readerError(error), error));
                }
            }
            if (!readRequirements(
                    reader,
                    names,
                    manifest.required_extensions,
                    manifest.required_components,
                    limits,
                    budget))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
            if (reader.remaining() != 0u)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::TRAILING_BYTES,
                    "LXSC contains trailing bytes"));
            }
            const auto valid = validateEntitySceneManifest(manifest, limits);
            if (!valid)
                return lux::cxx::unexpected(valid.error());
            return manifest;
    }
}
