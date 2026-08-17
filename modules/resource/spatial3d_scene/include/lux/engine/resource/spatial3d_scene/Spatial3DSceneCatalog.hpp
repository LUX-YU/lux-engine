#pragma once
/**
 * @file Spatial3DSceneCatalog.hpp
 * @brief Domain-owned finite 3D EntitySection catalog contribution config.
 */

#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>
#include <lux/engine/resource/spatial3d_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::spatial3d_scene
{
    inline constexpr std::string_view kSpatial3DContributionName =
        "org.lux.builtin.spatial3d.partitioned";
    inline constexpr std::string_view kSpatial3DResidentDemandChannelName =
        "lux.spatial3d.resident";
    inline constexpr std::string_view kSpatial3DVisualLodDemandChannelName =
        "lux.spatial3d.visual_lod";
    inline constexpr std::uint32_t kSpatial3DSceneCatalogMagic =
        0x4353334cu; // L3SC
    inline constexpr std::uint32_t kSpatial3DSceneCatalogSchemaVersion = 1u;

    struct Spatial3DSourceIdTag final {};
    using Spatial3DSourceId =
        lux::cxx::StableNameId<Spatial3DSourceIdTag>;

    struct Spatial3DSceneCatalogBand final
    {
        Spatial3DSourceId source;
        lux::entity_scene::DemandChannelId demand_channel;
        std::uint8_t level{0u};
        double cell_world_size{0.0};
        double active_distance_scale{1.0};
        double resident_distance_scale{1.0};

        friend bool operator==(
            const Spatial3DSceneCatalogBand&,
            const Spatial3DSceneCatalogBand&) = default;
    };

    struct Spatial3DSceneCatalogEntry final
    {
        lux::spatial::GridCoord3i64 coordinate;
        std::uint32_t band{0u};
        lux::entity_scene::EntitySectionId section;

        friend bool operator==(
            const Spatial3DSceneCatalogEntry&,
            const Spatial3DSceneCatalogEntry&) = default;
    };

    /// Fixed resident-set admission for one Scene.  This is deliberately
    /// independent of the number of catalogued Sections: adding distant
    /// content must not silently enlarge the live CPU/entity budget.
    struct Spatial3DResidencyCapacity final
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
            const Spatial3DResidencyCapacity&,
            const Spatial3DResidencyCapacity&) = default;
    };

    struct Spatial3DSceneCatalogConfig final
    {
        Spatial3DResidencyCapacity residency;
        std::vector<Spatial3DSceneCatalogBand> bands;
        std::vector<Spatial3DSceneCatalogEntry> entries;

        friend bool operator==(
            const Spatial3DSceneCatalogConfig&,
            const Spatial3DSceneCatalogConfig&) = default;
    };

    struct Spatial3DSceneCatalogCodecLimits final
    {
        std::uint32_t maximum_bands{4096u};
        std::uint32_t maximum_entries{1u << 24u};
        std::uint64_t maximum_encoded_bytes{512ull * 1024ull * 1024ull};
    };

    enum class ESpatial3DSceneCatalogError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        LIMIT_EXCEEDED,
        INVALID_BAND,
        INVALID_ENTRY,
        NON_CANONICAL_ORDER,
        DUPLICATE_LOCATION,
        DUPLICATE_SECTION,
        TRAILING_BYTES
    };

    struct Spatial3DSceneCatalogFailure final
    {
        ESpatial3DSceneCatalogError error{
            ESpatial3DSceneCatalogError::INVALID_ARGUMENT
        };
        std::string detail;
    };

    template <typename T>
    using Spatial3DSceneCatalogExp = lux::cxx::expected<T, Spatial3DSceneCatalogFailure>;

    [[nodiscard]] LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_PUBLIC
    Spatial3DSceneCatalogExp<void>
    validateSpatial3DSceneCatalog(
        const Spatial3DSceneCatalogConfig& config,
        const Spatial3DSceneCatalogCodecLimits& limits = {}) noexcept;

    /// Encoding canonicalizes a private copy. Decoding rejects non-canonical
    /// wire order, so one logical catalog has exactly one byte image.
    [[nodiscard]] LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_PUBLIC
    Spatial3DSceneCatalogExp<std::vector<std::byte>>
    encodeSpatial3DSceneCatalog(
        Spatial3DSceneCatalogConfig config,
        const Spatial3DSceneCatalogCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_PUBLIC
    Spatial3DSceneCatalogExp<Spatial3DSceneCatalogConfig>
    decodeSpatial3DSceneCatalog(
        std::span<const std::byte> bytes,
        const Spatial3DSceneCatalogCodecLimits& limits = {}) noexcept;
} // namespace lux::spatial3d_scene
