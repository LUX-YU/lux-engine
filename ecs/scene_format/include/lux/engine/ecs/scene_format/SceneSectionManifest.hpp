#pragma once
/**
 * @file SceneSectionManifest.hpp
 * @brief ECS-owned loading metadata for cooked EntitySection images.
 *
 * These types describe data consumed by EntitySection and streaming Systems.
 * They deliberately contain no Engine extension, renderer, authoring, or
 * runtime-execution concepts.
 */

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lux::ecs::scene_format
{
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
        return id.isValid() && isCanonicalStableName(id.name());
    }

    [[nodiscard]] inline bool isValidSectionGeneratorId(
        const SectionGeneratorId& id) noexcept
    {
        return id.isValid() && isCanonicalStableName(id.name());
    }

    enum class SectionCompression : std::uint8_t
    {
        NONE,
        ZSTD
    };

    struct RequiredComponentSchema final
    {
        lux::ecs::ComponentSchemaId id;
        std::uint32_t schema_version{0u};

        friend bool operator==(
            const RequiredComponentSchema&,
            const RequiredComponentSchema&) = default;
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

    using SectionSource = std::variant<
        StoredSectionSource,
        GeneratedSectionSource
    >;

    struct SectionRecord final
    {
        EntitySectionId                    id;
        SectionSource                      source;
        lux::cxx::algorithm::Sha256Digest  content_digest;
        SectionCompression                 compression{SectionCompression::NONE};
        std::uint64_t                      encoded_bytes{0u};
        std::uint64_t                      decoded_bytes{0u};
        std::uint32_t                      entity_count{0u};
        std::vector<EntitySectionId>       dependencies;
        std::vector<DemandChannelId>       demand_channels;
        std::vector<RequiredComponentSchema> required_components;

        friend bool operator==(
            const SectionRecord&,
            const SectionRecord&) = default;
    };
} // namespace lux::ecs::scene_format
