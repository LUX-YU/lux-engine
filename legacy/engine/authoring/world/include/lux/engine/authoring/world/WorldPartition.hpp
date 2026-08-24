#pragma once

#include <lux/engine/authoring/world/WorldIdentifiers.hpp>
#include <lux/engine/math/Position.hpp>

#include <cstdint>
#include <optional>
#include <variant>

namespace lux::authoring
{
    enum class EPartitionTopology : std::uint8_t
    {
        PLANAR_XY,
        PLANAR_XZ,
        VOLUMETRIC_XYZ
    };

    struct PlanarCellCoord final
    {
        std::int64_t a{0};
        std::int64_t b{0};

        friend bool operator==(
            const PlanarCellCoord&,
            const PlanarCellCoord&) = default;
    };

    struct VolumeCellCoord final
    {
        std::int64_t x{0};
        std::int64_t y{0};
        std::int64_t z{0};

        friend bool operator==(
            const VolumeCellCoord&,
            const VolumeCellCoord&) = default;
    };

    struct PlanarMacroCoord final
    {
        std::int64_t a{0};
        std::int64_t b{0};

        friend bool operator==(
            const PlanarMacroCoord&,
            const PlanarMacroCoord&) = default;
    };

    struct VolumeMacroCoord final
    {
        std::int64_t x{0};
        std::int64_t y{0};
        std::int64_t z{0};

        friend bool operator==(
            const VolumeMacroCoord&,
            const VolumeMacroCoord&) = default;
    };

    struct WorldCellKey final
    {
        EPartitionTopology topology{EPartitionTopology::PLANAR_XY};
        std::variant<PlanarCellCoord, VolumeCellCoord> coordinate{
            PlanarCellCoord{}};

        [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC bool valid() const
            noexcept;

        friend bool operator==(
            const WorldCellKey&,
            const WorldCellKey&) = default;
    };

    struct WorldMacroCoord final
    {
        EPartitionTopology topology{EPartitionTopology::PLANAR_XY};
        std::variant<PlanarMacroCoord, VolumeMacroCoord> coordinate{
            PlanarMacroCoord{}};

        [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC bool valid() const
            noexcept;

        friend bool operator==(
            const WorldMacroCoord&,
            const WorldMacroCoord&) = default;
    };

    struct PartitionSpaceDescriptor final
    {
        PartitionSpaceId id;
        EPartitionTopology topology{EPartitionTopology::PLANAR_XY};
        float cell_edge{128.0f};
        std::uint16_t macro_edge_cells{32u};

        friend bool operator==(
            const PartitionSpaceDescriptor&,
            const PartitionSpaceDescriptor&) = default;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC bool
    isValidCellEdge(float cell_edge) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    std::optional<WorldMacroCoord> macroCoordOf(
        const WorldCellKey& cell,
        std::uint16_t macro_edge_cells) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::optional<PlanarCellCoord>
    planarXyCellOf(
        const lux::math::Position2d& position,
        float cell_edge) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::optional<PlanarCellCoord>
    planarXzCellOf(
        const lux::math::Position3d& position,
        float cell_edge) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::optional<VolumeCellCoord>
    volumeCellOf(
        const lux::math::Position3d& position,
        float cell_edge) noexcept;
} // namespace lux::authoring
