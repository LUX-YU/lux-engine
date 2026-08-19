#pragma once
/**
 * @file Spatial2DSectionIndex.hpp
 * @brief Immutable 2D grid-to-EntitySection lookup for an infinite source.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>
#include <lux/engine/runtime/spatial2d/infinite/visibility.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace lux::runtime::spatial2d
{
    inline constexpr std::int64_t kSpatial2DActiveRadius = 1;
    inline constexpr std::int64_t kSpatial2DResidentRadius = 2;
    inline constexpr std::size_t kSpatial2DActiveSectionCount = 9u;
    inline constexpr std::size_t kSpatial2DResidentSectionCount = 25u;

    struct Spatial2DSectionIndexEntry final
    {
        lux::spatial::GridCoord2i64 coordinate;
        lux::ecs::scene_format::EntitySectionId section;

        friend bool operator==(
            const Spatial2DSectionIndexEntry&,
            const Spatial2DSectionIndexEntry&) = default;
    };

    struct Spatial2DWindowEntry final
    {
        lux::spatial::GridCoord2i64 coordinate;
        lux::ecs::scene_format::EntitySectionId section;
        /// Present only for procedurally addressed Sections. The generic
        /// partition owns this record with the demand source and releases it
        /// when that source moves or closes.
        std::optional<lux::scene::SectionRecord> record;
        bool active{false};
    };

    struct Spatial2DWindow final
    {
        lux::spatial::GridCoord2i64 center;
        std::array<
            Spatial2DWindowEntry,
            kSpatial2DResidentSectionCount> entries{};
    };

    enum class ESpatial2DIndexError : std::uint8_t
    {
        EMPTY_INDEX,
        INVALID_SECTION,
        DUPLICATE_COORDINATE,
        DUPLICATE_SECTION,
        INVALID_POSITION,
        COORDINATE_OVERFLOW,
        SECTION_NOT_FOUND
    };

    struct Spatial2DIndexFailure final
    {
        ESpatial2DIndexError code{ESpatial2DIndexError::EMPTY_INDEX};
        lux::spatial::GridCoord2i64 coordinate;
        lux::ecs::scene_format::EntitySectionId section;
    };

    /// Cheap position-to-grid conversion used by interest tracking before a
    /// procedural source allocates or hashes any Section records.
    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_PUBLIC
    lux::cxx::expected<
        lux::spatial::GridCoord2i64,
        Spatial2DIndexFailure>
    spatial2DSectionCoordinate(
        const lux::spatial::Position2D& position,
        double section_world_size) noexcept;

    /// Dimension-specific lookup kept outside spatial_partition.  The common
    /// planner only receives dimension-neutral Section IDs and priorities.
    class LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_PUBLIC
    Spatial2DSectionIndex final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            Spatial2DSectionIndex,
            Spatial2DIndexFailure>
        create(std::vector<Spatial2DSectionIndexEntry> entries);

        [[nodiscard]] const lux::ecs::scene_format::EntitySectionId* find(
            lux::spatial::GridCoord2i64 coordinate) const noexcept;

        [[nodiscard]] lux::cxx::expected<
            Spatial2DWindow,
            Spatial2DIndexFailure>
        window(
            const lux::spatial::Position2D& position,
            double section_world_size) const noexcept;

        [[nodiscard]] std::span<const Spatial2DSectionIndexEntry> entries()
            const noexcept
        {
            return entries_;
        }

    private:
        explicit Spatial2DSectionIndex(
            std::vector<Spatial2DSectionIndexEntry> entries) noexcept
            : entries_(std::move(entries))
        {}

        std::vector<Spatial2DSectionIndexEntry> entries_;
    };

    using Spatial2DSectionRecordFactory = lux::cxx::move_only_function<
        lux::cxx::expected<
            lux::scene::SectionRecord,
            Spatial2DIndexFailure>(lux::spatial::GridCoord2i64)>;

    /// One dimensional leaf source can be finite (cooked index) or
    /// procedural. Both produce the same window shape; only the procedural
    /// form supplies source-owned EntitySection records to the generic
    /// partition transaction.
    class LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_PUBLIC
    Spatial2DSectionSource final
    {
    public:
        [[nodiscard]] static Spatial2DSectionSource finite(Spatial2DSectionIndex index);
        [[nodiscard]] static lux::cxx::expected<
            Spatial2DSectionSource,
            Spatial2DIndexFailure>
        procedural(Spatial2DSectionRecordFactory factory);

        Spatial2DSectionSource(Spatial2DSectionSource&&) noexcept = default;
        Spatial2DSectionSource& operator=(
            Spatial2DSectionSource&&) noexcept = default;
        Spatial2DSectionSource(const Spatial2DSectionSource&) = delete;
        Spatial2DSectionSource& operator=(const Spatial2DSectionSource&) =
            delete;

        [[nodiscard]] lux::cxx::expected<
            Spatial2DWindow,
            Spatial2DIndexFailure>
        window(
            const lux::spatial::Position2D& position,
            double section_world_size);

    private:
        explicit Spatial2DSectionSource(
            std::optional<Spatial2DSectionIndex> finite,
            Spatial2DSectionRecordFactory factory) noexcept
            : finite_(std::move(finite)), factory_(std::move(factory))
        {}

        std::optional<Spatial2DSectionIndex> finite_;
        Spatial2DSectionRecordFactory factory_;
    };
}
