#pragma once
/**
 * @file SceneCatalog.hpp
 * @brief Engine-owned finite Spatial3D catalog and its stable L3SC codec.
 *
 * This contract indexes canonical ECS EntitySection identities and Engine
 * Scene demand channels. L3SC v1 is owned and implemented here; Resource does
 * not expose a parallel catalog contract.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/spatial3d/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::spatial3d
{
    inline constexpr std::string_view kPartitionedFeatureName =
        "org.lux.builtin.spatial3d.partitioned";
    inline constexpr std::string_view kResidentDemandChannelName =
        "lux.spatial3d.resident";
    inline constexpr std::string_view kVisualLodDemandChannelName =
        "lux.spatial3d.visual_lod";
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

    struct ResidencyCapacity final
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

        friend bool operator==(const ResidencyCapacity&, const ResidencyCapacity&) = default;
    };

    struct SceneCatalog final
    {
        ResidencyCapacity residency;
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

    [[nodiscard]] LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_PUBLIC
    SceneCatalogResult<void> validateSceneCatalog(
        const SceneCatalog& catalog,
        const SceneCatalogCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_PUBLIC
    SceneCatalogResult<std::vector<std::byte>> encodeSceneCatalog(
        SceneCatalog catalog,
        const SceneCatalogCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_PUBLIC
    SceneCatalogResult<SceneCatalog> decodeSceneCatalog(
        std::span<const std::byte> bytes,
        const SceneCatalogCodecLimits& limits = {}) noexcept;
} // namespace lux::spatial3d
