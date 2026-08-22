#pragma once
/**
 * @file SceneCatalog.hpp
 * @brief Cooked finite Spatial3D catalog and its stable L3SC codec.
 *
 * This format indexes canonical EntitySection identities by source, demand
 * channel, LOD band and cell. Runtime streaming policy is intentionally kept
 * outside this wire-format contract.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/ecs/scene_format/SceneSectionManifest.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/ecs/scene_format/spatial3d/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::ecs::scene_format::spatial3d
{
    inline constexpr std::uint32_t kSceneCatalogMagic =
        0x4353334cu; // L3SC
    inline constexpr std::uint32_t kSceneCatalogSchemaVersion = 1u;

    struct SourceIdTag final {};
    using SourceId = lux::cxx::StableNameId<SourceIdTag>;

    struct SceneCatalogBand final
    {
        SourceId source;
        lux::ecs::scene_format::DemandChannelId demand_channel;
        std::uint8_t level{0u};
        double cell_world_size{0.0};
        double active_distance_scale{1.0};
        double resident_distance_scale{1.0};

        friend bool operator==(const SceneCatalogBand&, const SceneCatalogBand&) =
            default;
    };

    struct SceneCatalogEntry final
    {
        lux::math::GridCoord3i64 coordinate;
        std::uint32_t band{0u};
        lux::ecs::scene_format::EntitySectionId section;

        friend bool operator==(const SceneCatalogEntry&, const SceneCatalogEntry&) = default;
    };

    /// Values serialized in the L3SC header. They become a streaming-domain
    /// ResidencyCapacity only after a product validates the cooked catalog.
    struct SceneCatalogResidencyLimits final
    {
        std::uint64_t maximum_decoded_bytes{1024ull * 1024ull * 1024ull};
        std::uint64_t maximum_entities{1'000'000u};
        std::uint32_t maximum_interest_sources{8u};
        std::uint32_t maximum_sections_per_interest{4096u};

        [[nodiscard]] bool valid() const noexcept
        {
            return maximum_decoded_bytes != 0u &&
                maximum_entities != 0u &&
                maximum_interest_sources != 0u &&
                maximum_sections_per_interest != 0u;
        }

        friend bool operator==(
            const SceneCatalogResidencyLimits&,
            const SceneCatalogResidencyLimits&) = default;
    };

    struct SceneCatalog final
    {
        SceneCatalogResidencyLimits residency;
        std::vector<SceneCatalogBand> bands;
        std::vector<SceneCatalogEntry> entries;

        friend bool operator==(const SceneCatalog&, const SceneCatalog&) = default;
    };

    struct SceneCatalogCodecLimits final
    {
        std::uint32_t maximum_bands{4096u};
        std::uint32_t maximum_entries{1u << 24u};
        std::uint64_t maximum_encoded_bytes{512ull * 1024ull * 1024ull};
    };

    enum class SceneCatalogError : std::uint8_t
    {
        InvalidArgument,
        BadMagic,
        UnsupportedVersion,
        Truncated,
        LimitExceeded,
        InvalidBand,
        InvalidEntry,
        NonCanonicalOrder,
        DuplicateLocation,
        DuplicateSection,
        TrailingBytes
    };

    struct SceneCatalogFailure final
    {
        SceneCatalogError error{SceneCatalogError::InvalidArgument};
        std::string detail;
    };

    template <class T>
    using SceneCatalogResult = lux::cxx::expected<T, SceneCatalogFailure>;

    [[nodiscard]] LUX_ECS_SPATIAL3D_SCENE_FORMAT_PUBLIC
    SceneCatalogResult<void> validateSceneCatalog(
        const SceneCatalog& catalog,
        const SceneCatalogCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ECS_SPATIAL3D_SCENE_FORMAT_PUBLIC
    SceneCatalogResult<std::vector<std::byte>> encodeSceneCatalog(
        SceneCatalog catalog,
        const SceneCatalogCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ECS_SPATIAL3D_SCENE_FORMAT_PUBLIC
    SceneCatalogResult<SceneCatalog> decodeSceneCatalog(
        std::span<const std::byte> bytes,
        const SceneCatalogCodecLimits& limits = {}) noexcept;
} // namespace lux::ecs::scene_format::spatial3d
