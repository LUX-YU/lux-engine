#pragma once
/**
 * @file SceneDescription.hpp
 * @brief Engine-owned cooked scene description model (LXSC v1 semantics).
 *
 * EntitySection image layout belongs to ecs::scene_format. This package adds
 * Engine concerns around those images: extension requirements, selected scene
 * features, storage/generation recipes and startup policy.
 */

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/scene/SceneFeatureId.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lux::scene
{
    inline constexpr std::uint32_t kSceneDescriptionMagic = 0x4353584cu;
    inline constexpr std::uint32_t kSceneDescriptionVersion = 1u;

    struct DemandChannelIdTag final {};
    struct SectionGeneratorIdTag final {};
    using DemandChannelId = lux::cxx::StableNameId<DemandChannelIdTag>;
    using SectionGeneratorId = lux::cxx::StableNameId<SectionGeneratorIdTag>;

    [[nodiscard]] constexpr lux::cxx::StableNameIdView<DemandChannelIdTag>
    demandChannelId(std::string_view name) noexcept
    {
        return lux::cxx::StableNameIdView<DemandChannelIdTag>{name};
    }

    [[nodiscard]] constexpr lux::cxx::StableNameIdView<SectionGeneratorIdTag>
    sectionGeneratorId(std::string_view name) noexcept
    {
        return lux::cxx::StableNameIdView<SectionGeneratorIdTag>{name};
    }

    [[nodiscard]] inline bool isValidDemandChannelId(
        const DemandChannelId& id) noexcept
    {
        return id.isValid() &&
            lux::ecs::scene_format::isCanonicalStableName(id.name());
    }

    [[nodiscard]] inline bool isValidSectionGeneratorId(
        const SectionGeneratorId& id) noexcept
    {
        return id.isValid() &&
            lux::ecs::scene_format::isCanonicalStableName(id.name());
    }

    enum class SectionCompression : std::uint8_t
    {
        None,
        Zstd
    };

    struct RequiredExtension final
    {
        lux::extensions::ExtensionId id;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};

        friend bool operator==(const RequiredExtension&, const RequiredExtension&) = default;
    };

    struct RequiredComponentSchema final
    {
        lux::ecs::ComponentSchemaId id;
        std::uint32_t schema_version{0u};

        friend bool operator==(const RequiredComponentSchema&, const RequiredComponentSchema&) = default;
    };

    struct SceneFeatureRequest final
    {
        SceneFeatureId id;
        std::uint32_t config_schema_version{0u};
        std::vector<std::byte> config;

        friend bool operator==(const SceneFeatureRequest&, const SceneFeatureRequest&) = default;
    };

    struct StoredSectionSource final
    {
        std::string content_path;
        friend bool operator==(const StoredSectionSource&, const StoredSectionSource&) = default;
    };

    struct GeneratedSectionSource final
    {
        SectionGeneratorId generator;
        std::uint64_t seed{0u};
        std::vector<std::byte> parameters;

        friend bool operator==(const GeneratedSectionSource&, const GeneratedSectionSource&) = default;
    };

    using SectionSource = std::variant<
        StoredSectionSource,
        GeneratedSectionSource
    >;

    struct SectionRecord final
    {
        lux::ecs::scene_format::EntitySectionId id;
        SectionSource                           source;
        lux::cxx::algorithm::Sha256Digest       content_digest;
        SectionCompression                      compression{SectionCompression::None};
        std::uint64_t                           encoded_bytes{0u};
        std::uint64_t                           decoded_bytes{0u};
        std::uint32_t                           entity_count{0u};
        std::vector<lux::ecs::scene_format::EntitySectionId> dependencies;
        std::vector<DemandChannelId>            demand_channels;
        std::vector<RequiredExtension>          required_extensions;
        std::vector<RequiredComponentSchema>    required_components;

        friend bool operator==(const SectionRecord&, const SectionRecord&) = default;
    };

    struct SceneDescription final
    {
        lux::asset::asset_id_t                  id;
        std::vector<SceneFeatureRequest>        features;
        std::vector<lux::ecs::scene_format::EntitySectionId> startup_sections;
        std::vector<SectionRecord>              sections;
        std::vector<RequiredExtension>          required_extensions;
        std::vector<RequiredComponentSchema>    required_components;

        friend bool operator==(const SceneDescription&, const SceneDescription&) = default;
    };
} // namespace lux::scene
