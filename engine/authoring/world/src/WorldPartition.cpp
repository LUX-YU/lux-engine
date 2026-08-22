#include <lux/engine/authoring/world/WorldPartition.hpp>

#include <cmath>
#include <limits>

namespace lux::authoring
{
    namespace
    {
        bool isPowerOfTwo(double value) noexcept
        {
            if (!std::isfinite(value) || value <= 0.0)
                return false;
            int exponent = 0;
            return std::frexp(value, &exponent) == 0.5;
        }

        std::int64_t floorDivide(
            std::int64_t value,
            std::int64_t positive_divisor) noexcept
        {
            auto quotient = value / positive_divisor;
            if (value % positive_divisor < 0)
                --quotient;
            return quotient;
        }

        std::optional<std::int64_t> cellAxis(
            double value,
            float cell_edge) noexcept
        {
            if (!std::isfinite(value) || !isValidCellEdge(cell_edge))
                return std::nullopt;

            const long double cell = std::floor(
                static_cast<long double>(value) /
                static_cast<long double>(cell_edge));
            if (cell < static_cast<long double>(
                           std::numeric_limits<std::int64_t>::min()) ||
                cell > static_cast<long double>(
                           std::numeric_limits<std::int64_t>::max()))
            {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(cell);
        }
    } // namespace

    bool WorldCellKey::valid() const noexcept
    {
        if (topology == EPartitionTopology::VOLUMETRIC_XYZ)
            return std::holds_alternative<VolumeCellCoord>(coordinate);
        return std::holds_alternative<PlanarCellCoord>(coordinate);
    }

    bool WorldMacroCoord::valid() const noexcept
    {
        if (topology == EPartitionTopology::VOLUMETRIC_XYZ)
            return std::holds_alternative<VolumeMacroCoord>(coordinate);
        return std::holds_alternative<PlanarMacroCoord>(coordinate);
    }

    bool isValidCellEdge(float cell_edge) noexcept
    {
        return std::isfinite(cell_edge) && cell_edge >= 1.0f &&
            isPowerOfTwo(cell_edge);
    }

    std::optional<WorldMacroCoord> macroCoordOf(
        const WorldCellKey& cell,
        std::uint16_t macro_edge_cells) noexcept
    {
        if (!cell.valid() || macro_edge_cells == 0u)
            return std::nullopt;
        const auto edge = static_cast<std::int64_t>(macro_edge_cells);
        WorldMacroCoord result;
        result.topology = cell.topology;
        if (const auto* planar =
                std::get_if<PlanarCellCoord>(&cell.coordinate))
        {
            result.coordinate = PlanarMacroCoord{
                floorDivide(planar->a, edge),
                floorDivide(planar->b, edge)};
            return result;
        }
        const auto& volume = std::get<VolumeCellCoord>(cell.coordinate);
        result.coordinate = VolumeMacroCoord{
            floorDivide(volume.x, edge),
            floorDivide(volume.y, edge),
            floorDivide(volume.z, edge)};
        return result;
    }

    std::optional<PlanarCellCoord> planarXyCellOf(
        const lux::math::Position2d& position,
        float cell_edge) noexcept
    {
        const auto x = cellAxis(position.x, cell_edge);
        const auto y = cellAxis(position.y, cell_edge);
        if (!x || !y)
            return std::nullopt;
        return PlanarCellCoord{*x, *y};
    }

    std::optional<PlanarCellCoord> planarXzCellOf(
        const lux::math::Position3d& position,
        float cell_edge) noexcept
    {
        const auto x = cellAxis(position.x, cell_edge);
        const auto z = cellAxis(position.z, cell_edge);
        if (!x || !z)
            return std::nullopt;
        return PlanarCellCoord{*x, *z};
    }

    std::optional<VolumeCellCoord> volumeCellOf(
        const lux::math::Position3d& position,
        float cell_edge) noexcept
    {
        const auto x = cellAxis(position.x, cell_edge);
        const auto y = cellAxis(position.y, cell_edge);
        const auto z = cellAxis(position.z, cell_edge);
        if (!x || !y || !z)
            return std::nullopt;
        return VolumeCellCoord{*x, *y, *z};
    }
} // namespace lux::authoring
