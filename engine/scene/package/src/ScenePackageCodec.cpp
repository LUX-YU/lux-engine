#include <lux/engine/scene/ScenePackageCodec.hpp>

#include "ScenePackageCodecCommon.hpp"

#include <algorithm>
#include <utility>

namespace lux::scene
{
    namespace
    {
        using namespace detail;

        [[nodiscard]] bool validFeatureId(const SceneFeatureId& id) noexcept
        {
            return id.isValid() && isValidSceneFeatureIdName(id.name());
        }

        void addRequirementsToNames(
            WireNameTable& names,
            std::span<const RequiredExtension> extensions,
            std::span<const RequiredComponentSchema> components)
        {
            for (const auto& value : extensions)
                names.add(value.id.name());
            for (const auto& value : components)
                names.add(value.id.name);
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
                writeStableId(writer, names, value.id);
                writer.u16(value.required_major);
                writer.u16(value.minimum_minor);
            }
            writer.u32(static_cast<std::uint32_t>(components.size()));
            for (const auto& value : components)
            {
                writeComponentId(writer, names, value.id);
                writer.u32(value.schema_version);
            }
        }

        [[nodiscard]] bool readRequirements(
            ByteReader& reader,
            const WireNameTable& names,
            std::vector<RequiredExtension>& extensions,
            std::vector<RequiredComponentSchema>& components,
            const ScenePackageCodecLimits& limits,
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
                if (!readStableId(
                        reader,
                        names,
                        value.id,
                        budget,
                        [](const auto& id)
                        {
                            return id.isValid() &&
                                lux::extensions::isCanonicalStableName(
                                    id.name());
                        }) ||
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
                if (!readComponentId(reader, names, value.id, budget) ||
                    !reader.u32(value.schema_version))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] WireNameTable packageNames(const ScenePackage& package)
        {
            WireNameTable names;
            for (const auto& feature : package.features)
                names.add(feature.id.name());
            addRequirementsToNames(
                names,
                package.required_extensions,
                package.required_components);
            for (const auto& section : package.sections)
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
            const SectionRecord& section)
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
            SectionRecord& section,
            const ScenePackageCodecLimits& limits,
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
                        reader,
                        names,
                        generated.generator,
                        budget,
                        isValidSectionGeneratorId) ||
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
            if (compression >
                static_cast<std::uint8_t>(SectionCompression::Zstd))
            {
                reader.fail("unsupported Section compression");
                return false;
            }
            section.compression = static_cast<SectionCompression>(compression);

            std::uint32_t count = 0u;
            if (!readCount(
                    reader,
                    limits.maximum_dependencies_per_section,
                    count,
                    "dependency count exceeds codec limit") ||
                !prepareVector(
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
                    "demand channel count exceeds codec limit") ||
                !prepareVector(
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
                if (!readStableId(
                        reader,
                        names,
                        channel,
                        budget,
                        isValidDemandChannelId))
                {
                    return false;
                }
            }
            return readRequirements(
                reader,
                names,
                section.required_extensions,
                section.required_components,
                limits,
                budget);
        }
    } // namespace

    ScenePackageCodecResult<std::vector<std::byte>> encodeScenePackage(
        const ScenePackage& package,
        const ScenePackageCodecLimits& limits) noexcept
    {
        const auto valid = validateScenePackage(package, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());

        const auto names = packageNames(package);
        if (names.size() > limits.maximum_names)
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::LimitExceeded,
                "LXSC structural name table exceeds codec limit"));
        }
        ByteWriter writer;
        writer.reserve(std::min<std::uint64_t>(
            limits.maximum_manifest_bytes, 64u * 1024u));
        writer.u32(kScenePackageMagic);
        writer.u32(kScenePackageVersion);
        names.write(writer);
        writeUuid(writer, package.id);

        writer.u32(static_cast<std::uint32_t>(package.features.size()));
        for (const auto& feature : package.features)
        {
            writeStableId(writer, names, feature.id);
            writer.u32(feature.config_schema_version);
            writeBlob(writer, feature.config);
        }
        writer.u32(static_cast<std::uint32_t>(
            package.startup_sections.size()));
        for (const auto& section : package.startup_sections)
            writeUuid(writer, section);
        writer.u32(static_cast<std::uint32_t>(package.sections.size()));
        for (const auto& section : package.sections)
            writeSectionRecord(writer, names, section);
        writeRequirements(
            writer,
            names,
            package.required_extensions,
            package.required_components);
        if (writer.size() > limits.maximum_manifest_bytes)
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::LimitExceeded,
                "encoded LXSC exceeds codec limit"));
        }
        return std::move(writer).take();
    }

    ScenePackageCodecResult<ScenePackage> decodeScenePackage(
        std::span<const std::byte> bytes,
        const ScenePackageCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_manifest_bytes)
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::LimitExceeded,
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
                ScenePackageCodecError::Truncated,
                "truncated LXSC header"));
        }
        if (magic != kScenePackageMagic)
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::BadMagic,
                "LXSC magic mismatch"));
        }
        if (version != kScenePackageVersion)
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::UnsupportedVersion,
                "unsupported LXSC version"));
        }
        WireNameTable names;
        if (!names.read(reader, limits, budget))
            return lux::cxx::unexpected(failure(readerError(error), error));

        ScenePackage package;
        if (!readUuid(reader, package.id))
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::Truncated,
                error));
        }
        std::uint32_t count = 0u;
        if (!readCount(
                reader,
                limits.maximum_features,
                count,
                "feature count exceeds codec limit") ||
            !prepareVector(
                reader,
                budget,
                package.features,
                count,
                20u,
                "features cannot fit remaining input",
                "features exceed decode allocation budget"))
        {
            return lux::cxx::unexpected(failure(readerError(error), error));
        }
        for (auto& feature : package.features)
        {
            if (!readStableId(
                    reader,
                    names,
                    feature.id,
                    budget,
                    validFeatureId) ||
                !reader.u32(feature.config_schema_version) ||
                !readBlob(
                    reader,
                    feature.config,
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
                "startup Section count exceeds codec limit") ||
            !prepareVector(
                reader,
                budget,
                package.startup_sections,
                count,
                16u,
                "startup Sections cannot fit remaining input",
                "startup Sections exceed decode allocation budget"))
        {
            return lux::cxx::unexpected(failure(readerError(error), error));
        }
        for (auto& section : package.startup_sections)
        {
            if (!readUuid(reader, section))
            {
                return lux::cxx::unexpected(failure(
                    ScenePackageCodecError::Truncated,
                    error));
            }
        }
        if (!readCount(
                reader,
                limits.maximum_sections,
                count,
                "EntitySection record count exceeds codec limit") ||
            !prepareVector(
                reader,
                budget,
                package.sections,
                count,
                90u,
                "EntitySection records cannot fit remaining input",
                "EntitySection records exceed decode allocation budget"))
        {
            return lux::cxx::unexpected(failure(readerError(error), error));
        }
        for (auto& section : package.sections)
        {
            if (!readSectionRecord(reader, names, section, limits, budget))
            {
                return lux::cxx::unexpected(failure(
                    readerError(error), error));
            }
        }
        if (!readRequirements(
                reader,
                names,
                package.required_extensions,
                package.required_components,
                limits,
                budget))
        {
            return lux::cxx::unexpected(failure(readerError(error), error));
        }
        if (reader.remaining() != 0u)
        {
            return lux::cxx::unexpected(failure(
                ScenePackageCodecError::TrailingBytes,
                "LXSC contains trailing bytes"));
        }
        const auto valid = validateScenePackage(package, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        return package;
    }
} // namespace lux::scene
