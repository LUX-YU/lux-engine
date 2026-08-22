#pragma once

#include <lux/engine/math/Position.hpp>

#include <lux/engine/authoring/world/visibility.h>
#include <lux/engine/authoring/world/WorldSource.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace lux::authoring
{
    enum class EWorldTerrainAuthoringError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_PAGE,
        MIXED_TERRAIN_SET,
        INCOMPLETE_REGION,
        SEAM_MISMATCH,
        IMAGE_DIMENSION_MISMATCH
    };

    struct WorldTerrainAuthoringFailure final
    {
        EWorldTerrainAuthoringError error{
            EWorldTerrainAuthoringError::INVALID_ARGUMENT};
        std::string detail;
    };

    enum class EWorldTerrainBrushMode : std::uint8_t
    {
        RAISE_LOWER,
        SMOOTH,
        FLATTEN,
        WEIGHT_PAINT,
        HOLE_PAINT
    };

    struct WorldTerrainBrush final
    {
        EWorldTerrainBrushMode mode{
            EWorldTerrainBrushMode::RAISE_LOWER};
        float radius{4.0f};
        /// 0 is a hard brush; 1 fades over the complete radius.
        float falloff{0.5f};
        /// RAISE_LOWER uses world units. SMOOTH/FLATTEN/WEIGHT use [0, 1].
        float strength{0.25f};
        float flatten_height{0.0f};
        std::uint8_t weight_layer{0u};
        std::uint8_t weight_value{255u};
        bool hole_value{true};
        /// Editing a seam without its neighbour would make a latent crack.
        /// Editor strokes keep this enabled and materialize all intersected
        /// pages before committing the transaction.
        bool require_complete_neighbourhood{true};
    };

    struct WorldTerrainEditTransaction final
    {
        std::vector<WorldTerrainPageDocument> before_pages;
        std::vector<WorldTerrainPageDocument> after_pages;
    };

    /// Applies one brush stroke to immutable LXTP snapshots. Shared samples
    /// use one canonical key, so a seam is calculated once and written to all
    /// participating pages. The caller commits all returned pages atomically.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldTerrainEditTransaction,
        WorldTerrainAuthoringFailure>
    applyWorldTerrainBrush(
        const WorldSourceDocument& root,
        std::span<const WorldTerrainPageDocument> pages,
        const lux::math::Position3d& center,
        const WorldTerrainBrush& brush);

    struct WorldTerrainHeightmap16 final
    {
        std::uint32_t width{0u};
        std::uint32_t height{0u};
        float height_min{0.0f};
        float height_max{1.0f};
        std::vector<std::uint16_t> samples;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        std::vector<std::byte>,
        WorldTerrainAuthoringFailure>
    encodeWorldTerrainRaw16(
        const WorldTerrainHeightmap16& image);

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldTerrainHeightmap16,
        WorldTerrainAuthoringFailure>
    decodeWorldTerrainRaw16(
        std::span<const std::byte> bytes,
        std::uint32_t width,
        std::uint32_t height,
        float height_min,
        float height_max);

    /// Exports a complete rectangular PLANAR_XZ page region. N pages along
    /// an axis produce N*256+1 samples; duplicated LXTP seam samples are
    /// validated and emitted once.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldTerrainHeightmap16,
        WorldTerrainAuthoringFailure>
    exportWorldTerrainHeightmap16(
        const WorldSourceDocument& root,
        std::span<const WorldTerrainPageDocument> pages);

    /// Imports into the same complete rectangular page region and returns an
    /// atomic before/after transaction suitable for the Editor undo stack.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldTerrainEditTransaction,
        WorldTerrainAuthoringFailure>
    importWorldTerrainHeightmap16(
        const WorldSourceDocument& root,
        std::span<const WorldTerrainPageDocument> pages,
        const WorldTerrainHeightmap16& image);
} // namespace lux::authoring
