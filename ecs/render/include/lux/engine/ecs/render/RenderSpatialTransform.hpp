#pragma once

#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace lux::ecs
{
    /// Render's fixed precision tile. It is an extraction detail, not an ECS
    /// coordinate contract and is intentionally not configurable per scene.
    inline constexpr double kRenderSpatialTileSize = 1024.0;

    [[nodiscard]] inline std::optional<std::int64_t> renderTileOf(
        double position) noexcept
    {
        if (!std::isfinite(position))
            return std::nullopt;
        const double tile = std::floor(position / kRenderSpatialTileSize);
        if (tile < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            tile > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(tile);
    }

    [[nodiscard]] inline std::optional<lux::spatial::GridCoord3i64>
    renderTileOf(const lux::spatial::Position3D& position) noexcept
    {
        const auto x = renderTileOf(position.x);
        const auto y = renderTileOf(position.y);
        const auto z = renderTileOf(position.z);
        if (!x || !y || !z)
            return std::nullopt;
        return lux::spatial::GridCoord3i64{*x, *y, *z};
    }

    [[nodiscard]] inline std::optional<lux::spatial::GridCoord3i64>
    renderTileOf(const lux::spatial::Position2D& position) noexcept
    {
        const auto x = renderTileOf(position.x);
        const auto y = renderTileOf(position.y);
        if (!x || !y)
            return std::nullopt;
        return lux::spatial::GridCoord3i64{*x, *y, 0};
    }

    namespace render_spatial_detail
    {
        inline bool splitAxis(
            double position,
            std::int64_t origin_tile,
            std::int32_t& tile_delta,
            float& local) noexcept
        {
            const auto tile = renderTileOf(position);
            if (!tile)
                return false;
            const bool positive = *tile >= origin_tile;
            const auto magnitude = positive
                ? static_cast<std::uint64_t>(*tile) -
                    static_cast<std::uint64_t>(origin_tile)
                : static_cast<std::uint64_t>(origin_tile) -
                    static_cast<std::uint64_t>(*tile);
            const auto maximum = positive
                ? static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max())
                : static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max()) + 1u;
            if (magnitude > maximum)
            {
                return false;
            }
            const auto delta = positive
                ? static_cast<std::int64_t>(magnitude)
                : -static_cast<std::int64_t>(magnitude);

            const double local_double = position -
                static_cast<double>(*tile) * kRenderSpatialTileSize;
            if (!std::isfinite(local_double) || local_double < 0.0 ||
                local_double >= kRenderSpatialTileSize)
            {
                return false;
            }
            const float local_float = static_cast<float>(local_double);
            if (!std::isfinite(local_float))
                return false;

            // A value immediately below the tile boundary can round to 1024
            // when narrowed to float. Carry it so the GPU representation stays
            // canonical.
            if (local_float >= static_cast<float>(kRenderSpatialTileSize))
            {
                if (delta == std::numeric_limits<std::int32_t>::max())
                    return false;
                tile_delta = static_cast<std::int32_t>(delta + 1);
                local = 0.0f;
            }
            else
            {
                tile_delta = static_cast<std::int32_t>(delta);
                local = local_float;
            }
            return true;
        }
    } // namespace render_spatial_detail

    [[nodiscard]] inline std::optional<lux::render::RenderLargePosition3D>
    makeRenderLargePosition(
        const lux::spatial::Position2D& position,
        const lux::spatial::GridCoord3i64& scene_origin_tile) noexcept
    {
        lux::render::RenderLargePosition3D result{};
        if (!render_spatial_detail::splitAxis(
                position.x,
                scene_origin_tile.x,
                result.page_delta[0],
                result.local[0]) ||
            !render_spatial_detail::splitAxis(
                position.y,
                scene_origin_tile.y,
                result.page_delta[1],
                result.local[1]))
        {
            return std::nullopt;
        }
        if (!render_spatial_detail::splitAxis(
                0.0,
                scene_origin_tile.z,
                result.page_delta[2],
                result.local[2]))
        {
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] inline std::optional<lux::render::RenderLargePosition3D>
    makeRenderLargePosition(
        const lux::spatial::Position3D& position,
        const lux::spatial::GridCoord3i64& scene_origin_tile) noexcept
    {
        lux::render::RenderLargePosition3D result{};
        if (!render_spatial_detail::splitAxis(
                position.x,
                scene_origin_tile.x,
                result.page_delta[0],
                result.local[0]) ||
            !render_spatial_detail::splitAxis(
                position.y,
                scene_origin_tile.y,
                result.page_delta[1],
                result.local[1]) ||
            !render_spatial_detail::splitAxis(
                position.z,
                scene_origin_tile.z,
                result.page_delta[2],
                result.local[2]))
        {
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] inline std::optional<lux::render::RenderSpatialTransform3D>
    makeRenderSpatialTransform(
        const ResolvedTransform3DComponent& transform,
        const lux::spatial::GridCoord3i64& scene_origin_tile) noexcept
    {
        const auto large_position = makeRenderLargePosition(
            transform.position,
            scene_origin_tile
        );
        if (!large_position)
            return std::nullopt;

        lux::render::RenderSpatialTransform3D result{};
        for (std::size_t column = 0; column != 3u; ++column)
        {
            result.basis_local[column * 4u + 0u] =
                transform.linear(0, static_cast<Eigen::Index>(column));
            result.basis_local[column * 4u + 1u] =
                transform.linear(1, static_cast<Eigen::Index>(column));
            result.basis_local[column * 4u + 2u] =
                transform.linear(2, static_cast<Eigen::Index>(column));
        }
        result.basis_local[3] = large_position->local[0];
        result.basis_local[7] = large_position->local[1];
        result.basis_local[11] = large_position->local[2];
        result.page_delta[0] = large_position->page_delta[0];
        result.page_delta[1] = large_position->page_delta[1];
        result.page_delta[2] = large_position->page_delta[2];
        return result;
    }
} // namespace lux::ecs
