#include <lux/engine/toolchain/spatial3d_scene/detail/Spatial3DNavigationCook.hpp>

#include <lux/engine/resource/terrain/TerrainTile.hpp>

#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace lux::toolchain::detail
{
    namespace
    {
        inline constexpr double kNavigationTileEdge = 64.0;
        inline constexpr int kMaximumVerticesPerArea = 6;

        struct HeightfieldDeleter final
        {
            void operator()(rcHeightfield* value) const noexcept
            {
                rcFreeHeightField(value);
            }
        };

        struct CompactHeightfieldDeleter final
        {
            void operator()(rcCompactHeightfield* value) const noexcept
            {
                rcFreeCompactHeightfield(value);
            }
        };

        struct ContourSetDeleter final
        {
            void operator()(rcContourSet* value) const noexcept
            {
                rcFreeContourSet(value);
            }
        };

        struct PolyMeshDeleter final
        {
            void operator()(rcPolyMesh* value) const noexcept
            {
                rcFreePolyMesh(value);
            }
        };

        [[nodiscard]] bool hole(
            const Spatial3DTerrainPageSource& page,
            std::size_t sample) noexcept
        {
            return ((page.holes[sample / 8u] >> (sample % 8u)) & 1u) != 0u;
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<lux::navigation::detour3d::
                NavigationTraversableArea3D>,
            std::string>
        cookTile(
            const Spatial3DTerrainPageSource& page,
            const Spatial3DNavigationAgentSource& agent,
            double cell_edge,
            std::uint32_t tile_x,
            std::uint32_t tile_z,
            std::uint32_t tiles_per_axis) noexcept
        {
            using lux::navigation::detour3d::NavigationTraversableArea3D;
            rcConfig config{};
            config.cs = agent.cell_size;
            config.ch = agent.cell_height;
            config.walkableSlopeAngle = agent.maximum_slope_degrees;
            const auto walkable_height = std::ceil(
                static_cast<double>(agent.height) / config.ch);
            const auto walkable_climb = std::floor(
                static_cast<double>(agent.maximum_climb) / config.ch);
            const auto walkable_radius = std::ceil(
                static_cast<double>(agent.radius) / config.cs);
            const auto maximum_edge_length =
                12.0 / static_cast<double>(config.cs);
            // Partition the whole Cell on one Recast lattice. A nominal
            // 64-unit tile is not generally divisible by cs (the default
            // 0.3 would rasterize to 64.2 units); restarting the lattice at
            // each nominal boundary leaves adjacent cooked layers without
            // identical polygon edges, so Detour cannot connect them.
            const auto cell_width = std::ceil(
                cell_edge / static_cast<double>(config.cs));
            const auto tileBoundary = [cell_width, tiles_per_axis](
                std::uint32_t index) noexcept
            {
                return std::floor(
                    cell_width *
                    (static_cast<double>(index) /
                     static_cast<double>(tiles_per_axis)));
            };
            const auto first_cell_x = tileBoundary(tile_x);
            const auto first_cell_z = tileBoundary(tile_z);
            const auto tile_cells_x =
                tileBoundary(tile_x + 1u) - first_cell_x;
            const auto tile_cells_z =
                tileBoundary(tile_z + 1u) - first_cell_z;
            const auto maximum_int = static_cast<double>(
                (std::numeric_limits<int>::max)());
            if (!std::isfinite(cell_width) ||
                !std::isfinite(tile_cells_x) ||
                !std::isfinite(tile_cells_z) || tile_cells_x < 1.0 ||
                tile_cells_z < 1.0 || walkable_height > maximum_int ||
                walkable_climb > maximum_int ||
                walkable_radius > (maximum_int - 3.0) * 0.5 ||
                maximum_edge_length > maximum_int ||
                tile_cells_x >
                    maximum_int - (walkable_radius + 3.0) * 2.0 ||
                tile_cells_z >
                    maximum_int - (walkable_radius + 3.0) * 2.0)
            {
                return lux::cxx::unexpected(std::string{
                    "navigation agent resolution exceeds Recast limits"});
            }
            const auto tile_start_x = static_cast<float>(
                first_cell_x * static_cast<double>(config.cs));
            const auto tile_start_z = static_cast<float>(
                first_cell_z * static_cast<double>(config.cs));
            const auto tile_edge_x = static_cast<float>(
                tile_cells_x * static_cast<double>(config.cs));
            const auto tile_edge_z = static_cast<float>(
                tile_cells_z * static_cast<double>(config.cs));
            config.walkableHeight = std::max(
                3, static_cast<int>(walkable_height));
            config.walkableClimb = static_cast<int>(walkable_climb);
            config.walkableRadius = static_cast<int>(walkable_radius);
            config.maxEdgeLen = static_cast<int>(maximum_edge_length);
            config.maxSimplificationError = 1.3f;
            config.minRegionArea = 64;
            config.mergeRegionArea = 400;
            config.maxVertsPerPoly = kMaximumVerticesPerArea;
            config.detailSampleDist = config.cs * 6.0f;
            config.detailSampleMaxError = config.ch;
            config.borderSize = config.walkableRadius + 3;
            config.tileSize = static_cast<int>(std::max(
                tile_cells_x, tile_cells_z));
            config.width = static_cast<int>(tile_cells_x) +
                config.borderSize * 2;
            config.height = static_cast<int>(tile_cells_z) +
                config.borderSize * 2;
            const auto border_world = config.borderSize * config.cs;
            config.bmin[0] = -border_world;
            config.bmin[1] = page.height_min;
            config.bmin[2] = -border_world;
            config.bmax[0] = tile_edge_x + border_world;
            config.bmax[1] = page.height_max + config.ch * 2.0f;
            config.bmax[2] = tile_edge_z + border_world;

            const auto first_x = static_cast<std::uint32_t>(std::max(
                0.0,
                std::floor(static_cast<double>(
                    (tile_start_x - border_world) /
                    page.sample_spacing))));
            const auto first_z = static_cast<std::uint32_t>(std::max(
                0.0,
                std::floor(static_cast<double>(
                    (tile_start_z - border_world) /
                    page.sample_spacing))));
            const auto last_x = static_cast<std::uint32_t>(std::min(
                static_cast<double>(lux::terrain::kTerrainTileQuadEdge),
                std::ceil(static_cast<double>(
                    (tile_start_x + tile_edge_x + border_world) /
                    page.sample_spacing))));
            const auto last_z = static_cast<std::uint32_t>(std::min(
                static_cast<double>(lux::terrain::kTerrainTileQuadEdge),
                std::ceil(static_cast<double>(
                    (tile_start_z + tile_edge_z + border_world) /
                    page.sample_spacing))));
            const auto width = last_x - first_x + 1u;
            const auto depth = last_z - first_z + 1u;
            std::vector<float> vertices;
            vertices.reserve(
                static_cast<std::size_t>(width) * depth * 3u);
            for (std::uint32_t z = first_z; z <= last_z; ++z)
            {
                for (std::uint32_t x = first_x; x <= last_x; ++x)
                {
                    const auto sample = static_cast<std::size_t>(z) *
                        lux::terrain::kTerrainTileSampleEdge + x;
                    vertices.push_back(
                        x * page.sample_spacing - tile_start_x);
                    vertices.push_back(page.heights[sample]);
                    vertices.push_back(
                        z * page.sample_spacing - tile_start_z);
                }
            }

            std::vector<int> triangles;
            triangles.reserve(
                static_cast<std::size_t>(width - 1u) *
                (depth - 1u) * 6u);
            for (std::uint32_t z = first_z; z < last_z; ++z)
            {
                for (std::uint32_t x = first_x; x < last_x; ++x)
                {
                    const auto source = static_cast<std::size_t>(z) *
                        lux::terrain::kTerrainTileSampleEdge + x;
                    if (hole(page, source) || hole(page, source + 1u) ||
                        hole(
                            page,
                            source +
                                lux::terrain::kTerrainTileSampleEdge) ||
                        hole(
                            page,
                            source +
                                lux::terrain::kTerrainTileSampleEdge + 1u))
                    {
                        continue;
                    }
                    const auto local = static_cast<int>(
                        (z - first_z) * width + (x - first_x));
                    const auto row = static_cast<int>(width);
                    triangles.insert(
                        triangles.end(),
                        {local,
                         local + row,
                         local + 1,
                         local + 1,
                         local + row,
                         local + row + 1});
                }
            }
            if (triangles.empty())
                return std::vector<NavigationTraversableArea3D>{};

            rcContext context{false};
            std::unique_ptr<rcHeightfield, HeightfieldDeleter> heightfield{
                rcAllocHeightfield()};
            if (!heightfield || !rcCreateHeightfield(
                    &context,
                    *heightfield,
                    config.width,
                    config.height,
                    config.bmin,
                    config.bmax,
                    config.cs,
                    config.ch))
            {
                return lux::cxx::unexpected(
                    std::string{"Recast heightfield allocation failed"});
            }
            std::vector<unsigned char> areas(triangles.size() / 3u);
            rcMarkWalkableTriangles(
                &context,
                config.walkableSlopeAngle,
                vertices.data(),
                static_cast<int>(vertices.size() / 3u),
                triangles.data(),
                static_cast<int>(areas.size()),
                areas.data());
            if (!rcRasterizeTriangles(
                    &context,
                    vertices.data(),
                    static_cast<int>(vertices.size() / 3u),
                    triangles.data(),
                    areas.data(),
                    static_cast<int>(areas.size()),
                    *heightfield,
                    config.walkableClimb))
            {
                return lux::cxx::unexpected(
                    std::string{"Recast triangle rasterization failed"});
            }
            rcFilterLowHangingWalkableObstacles(
                &context, config.walkableClimb, *heightfield);
            rcFilterLedgeSpans(
                &context,
                config.walkableHeight,
                config.walkableClimb,
                *heightfield);
            rcFilterWalkableLowHeightSpans(
                &context, config.walkableHeight, *heightfield);

            std::unique_ptr<rcCompactHeightfield,
                            CompactHeightfieldDeleter>
                compact{rcAllocCompactHeightfield()};
            std::unique_ptr<rcContourSet, ContourSetDeleter> contours{
                rcAllocContourSet()};
            std::unique_ptr<rcPolyMesh, PolyMeshDeleter> mesh{
                rcAllocPolyMesh()};
            if (!compact || !contours || !mesh ||
                !rcBuildCompactHeightfield(
                    &context,
                    config.walkableHeight,
                    config.walkableClimb,
                    *heightfield,
                    *compact) ||
                !rcErodeWalkableArea(
                    &context, config.walkableRadius, *compact) ||
                !rcBuildDistanceField(&context, *compact) ||
                !rcBuildRegions(
                    &context,
                    *compact,
                    config.borderSize,
                    config.minRegionArea,
                    config.mergeRegionArea) ||
                !rcBuildContours(
                    &context,
                    *compact,
                    config.maxSimplificationError,
                    config.maxEdgeLen,
                    *contours) ||
                !rcBuildPolyMesh(
                    &context,
                    *contours,
                    config.maxVertsPerPoly,
                    *mesh))
            {
                return lux::cxx::unexpected(
                    std::string{"Recast navigation topology build failed"});
            }

            std::vector<NavigationTraversableArea3D> result;
            result.reserve(static_cast<std::size_t>(mesh->npolys));
            const auto world_x = std::fma(
                static_cast<double>(page.cell.x), cell_edge,
                static_cast<double>(tile_start_x + mesh->bmin[0]));
            const auto world_z = std::fma(
                static_cast<double>(page.cell.z), cell_edge,
                static_cast<double>(tile_start_z + mesh->bmin[2]));
            if (!std::isfinite(world_x) || !std::isfinite(world_z))
            {
                return lux::cxx::unexpected(
                    std::string{"navigation position exceeds double range"});
            }
            for (int polygon = 0; polygon < mesh->npolys; ++polygon)
            {
                NavigationTraversableArea3D area;
                area.area_class = mesh->areas[polygon];
                area.traversal_flags = 1u;
                const auto base = static_cast<std::size_t>(polygon) *
                    static_cast<std::size_t>(mesh->nvp) * 2u;
                for (int corner = 0; corner < mesh->nvp; ++corner)
                {
                    const auto vertex = mesh->polys[
                        base + static_cast<std::size_t>(corner)];
                    if (vertex == RC_MESH_NULL_IDX)
                        break;
                    area.boundary.push_back({
                        world_x +
                            static_cast<double>(mesh->verts[vertex * 3u]) *
                                mesh->cs,
                        static_cast<double>(mesh->bmin[1]) +
                            static_cast<double>(
                                mesh->verts[vertex * 3u + 1u]) * mesh->ch,
                        world_z +
                            static_cast<double>(
                                mesh->verts[vertex * 3u + 2u]) * mesh->cs});
                }
                if (area.boundary.size() >= 3u)
                    result.push_back(std::move(area));
            }
            return result;
        }
    } // namespace

    lux::cxx::expected<
        lux::navigation::detour3d::NavigationRegion3DDescription,
        std::string>
    cookSpatial3DNavigationRegion(
        const Spatial3DTerrainPageSource& terrain,
        const Spatial3DNavigationAgentSource& agent,
        lux::navigation::NavigationRegionId region,
        double cell_edge) noexcept
    {
        using namespace lux::navigation;
        using namespace lux::navigation::detour3d;
        const NavigationAgentConstraints constraints{
            agent.radius,
            agent.height,
            agent.maximum_climb,
            agent.maximum_slope_degrees};
        if (!region.valid() || !valid(constraints) ||
            !(agent.maximum_slope_degrees > 0.0f) ||
            !std::isfinite(agent.cell_size) || agent.cell_size <= 0.0f ||
            !std::isfinite(agent.cell_height) ||
            agent.cell_height <= 0.0f || terrain.cell.y != 0 ||
            !std::isfinite(cell_edge) || cell_edge <= 0.0 ||
            !std::isfinite(terrain.height_min) ||
            !std::isfinite(terrain.height_max) ||
            !(terrain.height_max > terrain.height_min) ||
            !std::isfinite(terrain.sample_spacing) ||
            terrain.sample_spacing <= 0.0f ||
            terrain.heights.size() !=
                lux::terrain::kTerrainTileSampleCount ||
            terrain.holes.size() != lux::terrain::kTerrainTileHoleBytes ||
            !std::ranges::all_of(
                terrain.heights,
                [](float value) { return std::isfinite(value); }))
        {
            return lux::cxx::unexpected(
                std::string{"navigation Terrain or agent input is invalid"});
        }
        const auto sample_edge = static_cast<double>(
            lux::terrain::kTerrainTileQuadEdge) * terrain.sample_spacing;
        const auto tile_ratio = cell_edge / kNavigationTileEdge;
        if (!std::isfinite(tile_ratio) || tile_ratio < 1.0 ||
            tile_ratio > static_cast<double>(
                (std::numeric_limits<std::uint32_t>::max)()))
        {
            return lux::cxx::unexpected(
                std::string{"navigation Cell tile count is out of range"});
        }
        const auto tiles_per_axis = static_cast<std::uint32_t>(
            std::llround(tile_ratio));
        if (tiles_per_axis == 0u ||
            std::abs(sample_edge - cell_edge) > 1.0e-4 ||
            std::abs(static_cast<double>(tiles_per_axis) *
                         kNavigationTileEdge -
                     cell_edge) > 1.0e-4)
        {
            return lux::cxx::unexpected(std::string{
                "navigation Cell must align to Terrain samples and 64-unit build tiles"});
        }

        NavigationRegion3DDescription result;
        result.region = region;
        result.agent = constraints;
        result.horizontal_resolution = agent.cell_size;
        result.vertical_resolution = agent.cell_height;
        for (std::uint32_t z = 0u; z < tiles_per_axis; ++z)
        {
            for (std::uint32_t x = 0u; x < tiles_per_axis; ++x)
            {
                auto areas = cookTile(
                    terrain,
                    agent,
                    cell_edge,
                    x,
                    z,
                    tiles_per_axis);
                if (!areas)
                    return lux::cxx::unexpected(std::move(areas.error()));
                result.areas.insert(
                    result.areas.end(),
                    std::make_move_iterator(areas->begin()),
                    std::make_move_iterator(areas->end()));
            }
        }
        if (result.areas.empty())
        {
            return lux::cxx::unexpected(
                std::string{"navigation Terrain produced no traversable area"});
        }
        return result;
    }
} // namespace lux::toolchain::detail
