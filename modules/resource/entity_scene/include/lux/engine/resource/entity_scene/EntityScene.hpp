#pragma once
/**
 * @file EntityScene.hpp
 * @brief Dimension- and domain-neutral cooked scene manifest contract.
 */

#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>

#include <lux/engine/core/extension_abi/StableId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lux::entity_scene
{
    inline constexpr std::uint32_t kEntitySceneManifestMagic = 0x4353584cu;
    inline constexpr std::uint32_t kEntitySceneManifestVersion = 1u;

    enum class EEntitySectionCompression : std::uint8_t
    {
        NONE,
        ZSTD
    };

    struct RequiredExtension final
    {
        lux::extensions::ExtensionId id;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};

        friend bool operator==(
            const RequiredExtension&,
            const RequiredExtension&) = default;
    };

    struct RequiredComponentSchema final
    {
        ComponentSchemaId id;
        std::uint32_t schema_version{0u};

        friend bool operator==(
            const RequiredComponentSchema&,
            const RequiredComponentSchema&) = default;
    };

    struct SceneContribution final
    {
        lux::extensions::ContributionId id;
        std::uint32_t config_schema_version{0u};
        std::vector<std::byte> config;

        friend bool operator==(
            const SceneContribution&,
            const SceneContribution&) = default;
    };

    struct StoredSectionSource final
    {
        std::string content_path;

        friend bool operator==(
            const StoredSectionSource&,
            const StoredSectionSource&) = default;
    };

    struct GeneratedSectionSource final
    {
        SectionGeneratorId generator;
        std::uint64_t seed{0u};
        std::vector<std::byte> parameters;

        friend bool operator==(
            const GeneratedSectionSource&,
            const GeneratedSectionSource&) = default;
    };

    using EntitySectionSource = std::variant<
        StoredSectionSource,
        GeneratedSectionSource>;

    struct EntitySectionRecord final
    {
        EntitySectionId id;
        EntitySectionSource source;
        // SHA-256 of the decoded LXES image. Compression and storage location
        // therefore do not change logical Section content identity.
        lux::cxx::algorithm::Sha256Digest content_digest;
        EEntitySectionCompression compression{
            EEntitySectionCompression::NONE};
        std::uint64_t encoded_bytes{0u};
        std::uint64_t decoded_bytes{0u};
        std::uint32_t entity_count{0u};
        // Every list below is strictly sorted by stable identity and contains
        // no duplicates. Dependencies are local to this manifest and acyclic.
        std::vector<EntitySectionId> dependencies;
        std::vector<DemandChannelId> demand_channels;
        std::vector<RequiredExtension> required_extensions;
        std::vector<RequiredComponentSchema> required_components;

        friend bool operator==(
            const EntitySectionRecord&,
            const EntitySectionRecord&) = default;
    };

    struct EntitySceneManifest final
    {
        EntitySceneId id;
        // Canonical ordering is part of LXSC: names sort bytewise, UUID-backed
        // Section lists sort by their 16 wire bytes.
        std::vector<SceneContribution> contributions;
        std::vector<EntitySectionId> startup_sections;
        std::vector<EntitySectionRecord> sections;
        std::vector<RequiredExtension> required_extensions;
        std::vector<RequiredComponentSchema> required_components;

        friend bool operator==(
            const EntitySceneManifest&,
            const EntitySceneManifest&) = default;
    };
}
