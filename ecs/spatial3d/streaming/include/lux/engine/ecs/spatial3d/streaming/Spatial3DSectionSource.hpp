#pragma once
/**
 * @file Spatial3DSectionSource.hpp
 * @brief 3D cell-to-EntitySection record catalogs and rule grids.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/ecs/scene_format/SceneSectionManifest.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/ecs/spatial3d/streaming/visibility.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs::spatial3d::streaming
{
    struct Spatial3DSectionCatalogEntry final
    {
        lux::math::GridCoord3i64 coordinate;
        lux::ecs::scene_format::EntitySectionId section;
    };

    struct Spatial3DWindowEntry final
    {
        lux::math::GridCoord3i64 coordinate;
        lux::ecs::scene_format::EntitySectionId section;
        bool active{false};
    };

    struct Spatial3DWindow final
    {
        lux::math::GridCoord3i64 center;
        lux::math::GridCoord3i64 predicted_center;
        std::vector<Spatial3DWindowEntry> entries;
        /// Rule-grid records travel beside the compact spatial entries and
        /// are moved into one prospective partition transaction. Catalog
        /// windows leave this empty because their records already live in the
        /// immutable SceneDescription SectionRecord span.
        std::vector<lux::ecs::scene_format::SectionRecord> records;
        std::size_t active_sections{0u};
    };

    struct Spatial3DWindowRequest final
    {
        lux::math::Position3d center;
        double cell_world_size{64.0};
        double active_distance{64.0};
        double resident_distance{128.0};
        double prediction_offset_x{0.0};
        double prediction_offset_y{0.0};
        double prediction_offset_z{0.0};
        std::size_t maximum_sections{4096u};
    };

    enum class ESpatial3DSourceError : std::uint8_t
    {
        EMPTY_CATALOG,
        INVALID_RECORD,
        DUPLICATE_COORDINATE,
        DUPLICATE_SECTION,
        INVALID_REQUEST,
        COORDINATE_OVERFLOW,
        WINDOW_LIMIT_EXCEEDED,
        SECTION_NOT_FOUND,
        RULE_FAILURE
    };

    struct Spatial3DSourceFailure final
    {
        ESpatial3DSourceError code{ESpatial3DSourceError::INVALID_REQUEST};
        lux::math::GridCoord3i64 coordinate;
        lux::ecs::scene_format::EntitySectionId section;
        std::size_t requested_sections{0u};
        std::size_t maximum_sections{0u};
    };

    [[nodiscard]] LUX_ECS_SPATIAL3D_STREAMING_PUBLIC
    lux::cxx::expected<
        lux::math::GridCoord3i64,
        Spatial3DSourceFailure>
    spatial3DSectionCoordinate(
        const lux::math::Position3d& position,
        double cell_world_size) noexcept;

    /// A finite cooked grid. Records remain exclusively owned by the scene's
    /// SceneDescription; this leaf stores only coordinate-to-Section keys.
    class LUX_ECS_SPATIAL3D_STREAMING_PUBLIC
    Spatial3DSectionCatalog final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            Spatial3DSectionCatalog,
            Spatial3DSourceFailure>
        create(std::vector<Spatial3DSectionCatalogEntry> entries);

        [[nodiscard]] const Spatial3DSectionCatalogEntry* find(
            lux::math::GridCoord3i64 coordinate) const noexcept;
        [[nodiscard]] std::span<const Spatial3DSectionCatalogEntry> entries()
            const noexcept
        {
            return entries_;
        }

    private:
        explicit Spatial3DSectionCatalog(
            std::vector<Spatial3DSectionCatalogEntry> entries) noexcept
            : entries_(std::move(entries))
        {}

        std::vector<Spatial3DSectionCatalogEntry> entries_;
    };

    using Spatial3DSectionRecordRule = lux::cxx::move_only_function<
        lux::cxx::expected<
            lux::ecs::scene_format::SectionRecord,
            Spatial3DSourceFailure>(lux::math::GridCoord3i64)>;

    /// A leaf source is either a finite cooked catalog or an unbounded rule
    /// grid. Both expose the same window and feed dimension-neutral demands.
    class LUX_ECS_SPATIAL3D_STREAMING_PUBLIC
    Spatial3DSectionSource final
    {
    public:
        [[nodiscard]] static Spatial3DSectionSource catalog(Spatial3DSectionCatalog catalog);
        [[nodiscard]] static lux::cxx::expected<
            Spatial3DSectionSource,
            Spatial3DSourceFailure>
        ruleGrid(Spatial3DSectionRecordRule rule);

        Spatial3DSectionSource(Spatial3DSectionSource&&) noexcept = default;
        Spatial3DSectionSource& operator=(
            Spatial3DSectionSource&&) noexcept = default;
        Spatial3DSectionSource(const Spatial3DSectionSource&) = delete;
        Spatial3DSectionSource& operator=(const Spatial3DSectionSource&) =
            delete;

        [[nodiscard]] lux::cxx::expected<
            Spatial3DWindow,
            Spatial3DSourceFailure>
        window(const Spatial3DWindowRequest& request);

    private:
        explicit Spatial3DSectionSource(
            std::optional<Spatial3DSectionCatalog> catalog,
            Spatial3DSectionRecordRule rule) noexcept
            : catalog_(std::move(catalog)), rule_(std::move(rule))
        {}

        std::optional<Spatial3DSectionCatalog> catalog_;
        Spatial3DSectionRecordRule rule_;
    };
}
