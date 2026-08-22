#include <lux/engine/navigation/detour3d/detail/NavigationDetour3DInternal.hpp>

#include <lux/cxx/binary/Binary.hpp>

#include <DetourAlloc.h>
#include <DetourNavMeshBuilder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <limits>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::navigation::detour3d
{
    namespace
    {
        constexpr std::uint32_t kRegionMagic = 0x524e584cu; // LXNR
        constexpr std::size_t kMaximumAreas = 65'535u;
        constexpr std::size_t kMaximumPortals = 65'535u;
        constexpr std::size_t kMaximumBoundaryPoints = 6u;
        constexpr std::size_t kAreasPerLayer = 64u;
        template <class Value>
        void writeBinary(lux::cxx::BinaryVectorWriter& writer, Value value)
        {
            if constexpr (std::is_floating_point_v<Value>)
            {
                static_cast<void>(writer.writeFloat(
                    value,
                    lux::cxx::EFloatingPointPolicy::PRESERVE_BITS
                ));
            }
            else if constexpr (std::is_signed_v<Value>)
            {
                static_cast<void>(writer.writeSigned(value));
            }
            else
            {
                static_cast<void>(writer.writeUnsigned(value));
            }
        }

        template <class Value>
        [[nodiscard]] bool readBinary(
            lux::cxx::BinaryReader& reader,
            Value& value
        ) noexcept
        {
            if constexpr (std::is_floating_point_v<Value>)
            {
                return reader.readFloat(
                    value,
                    lux::cxx::EFloatingPointPolicy::PRESERVE_BITS
                );
            }
            else if constexpr (std::is_signed_v<Value>)
            {
                return reader.readSigned(value);
            }
            else
            {
                return reader.readUnsigned(value);
            }
        }

        struct QuantizedPoint final
        {
            std::uint16_t x{0u};
            std::uint16_t y{0u};
            std::uint16_t z{0u};

            friend bool operator==(const QuantizedPoint&,
                                   const QuantizedPoint&) = default;
        };

        struct QuantizedPointHash final
        {
            [[nodiscard]] std::size_t
            operator()(QuantizedPoint value) const noexcept
            {
                const auto packed = static_cast<std::uint64_t>(value.x) |
                                    (static_cast<std::uint64_t>(value.y) <<
                                     16u) |
                                    (static_cast<std::uint64_t>(value.z) <<
                                     32u);
                return static_cast<std::size_t>(
                    packed ^ (packed >> 29u) ^ (packed << 17u));
            }
        };

        struct UndirectedEdge final
        {
            std::uint16_t first{0u};
            std::uint16_t second{0u};

            friend bool operator==(const UndirectedEdge&,
                                   const UndirectedEdge&) = default;
        };

        struct UndirectedEdgeHash final
        {
            [[nodiscard]] std::size_t
            operator()(UndirectedEdge value) const noexcept
            {
                return static_cast<std::size_t>(value.first) |
                       (static_cast<std::size_t>(value.second) << 16u);
            }
        };

        struct EdgeUse final
        {
            std::size_t area{0u};
            std::size_t edge{0u};
            std::uint16_t from{0u};
            std::uint16_t to{0u};
            bool paired{false};
        };

        struct DecodedArea final
        {
            std::vector<lux::math::Position3d> boundary;
            std::uint8_t area_class{0u};
            std::uint16_t traversal_flags{1u};
        };

        struct DecodedRegion final
        {
            NavigationRegionId region;
            NavigationAgentConstraints agent;
            float horizontal_resolution{0.0f};
            float vertical_resolution{0.0f};
            std::vector<DecodedArea> areas;
            std::vector<NavigationPortal> portals;
        };

        [[nodiscard]] NavigationRegion3DFailure
        fail(ENavigationRegion3DError code, std::string detail)
        {
            return NavigationRegion3DFailure{code, std::move(detail)};
        }

        [[nodiscard]] bool finitePositive(float value) noexcept
        {
            return std::isfinite(value) && value > 0.0f;
        }

        [[nodiscard]] bool
        validDescription(const NavigationRegion3DDescription& value) noexcept
        {
            if (!value.region.valid() || !valid(value.agent) ||
                !finitePositive(value.horizontal_resolution) ||
                !finitePositive(value.vertical_resolution) ||
                value.areas.empty() || value.areas.size() > kMaximumAreas)
            {
                return false;
            }
            for (const auto& area : value.areas)
            {
                if (area.boundary.size() < 3u ||
                    area.boundary.size() > kMaximumBoundaryPoints ||
                    area.traversal_flags == 0u ||
                    !std::ranges::all_of(
                        area.boundary,
                        [](const auto& point)
                        { return lux::math::isFinite(point); }))
                {
                    return false;
                }
                // Subtract a local origin before the cross products.  Direct
                // x*z terms catastrophically cancel at large world
                // coordinates even though the region itself is small.
                const auto& origin = area.boundary.front();
                double signed_area = 0.0;
                for (std::size_t index = 1u; index + 1u < area.boundary.size();
                     ++index)
                {
                    const auto& first = area.boundary[index];
                    const auto& second = area.boundary[index + 1u];
                    signed_area +=
                        (first.x - origin.x) * (second.z - origin.z) -
                        (second.x - origin.x) * (first.z - origin.z);
                }
                if (!std::isfinite(signed_area) ||
                    std::abs(signed_area) <= 1.0e-12)
                {
                    return false;
                }
            }
            if (value.portals.size() > kMaximumPortals)
                return false;
            for (std::size_t index = 0u; index < value.portals.size(); ++index)
            {
                const auto& portal = value.portals[index];
                if (!portal.id.valid() || !portal.first_region.valid() ||
                    !portal.second_region.valid() ||
                    portal.first_region == portal.second_region ||
                    (portal.first_region != value.region &&
                     portal.second_region != value.region) ||
                    !lux::math::isFinite(portal.first_position) ||
                    !lux::math::isFinite(portal.second_position) ||
                    !finitePositive(portal.traversal_cost_scale))
                {
                    return false;
                }
                for (std::size_t previous = 0u; previous < index; ++previous)
                    if (value.portals[previous].id == portal.id)
                        return false;
            }
            return true;
        }

        [[nodiscard]] lux::cxx::expected<DecodedRegion,
                                         NavigationRegion3DFailure>
        decodeRegion(const NavigationRegion3DBlob& blob) noexcept
        {
            if (!blob.valid())
            {
                return lux::cxx::unexpected(
                    fail(ENavigationRegion3DError::INVALID_REQUEST,
                         "navigation region blob is incomplete"));
            }
            lux::cxx::BinaryReader reader{blob.payload.view()};
            std::uint32_t magic = 0u;
            std::uint32_t version = 0u;
            DecodedRegion result;
            std::uint32_t area_count = 0u;
            std::uint32_t portal_count = 0u;
            if (!readBinary(reader, magic) || !readBinary(reader, version) ||
                !readBinary(reader, result.region.high) ||
                !readBinary(reader, result.region.low) ||
                !readBinary(reader, result.agent.radius) ||
                !readBinary(reader, result.agent.height) ||
                !readBinary(reader, result.agent.maximum_climb) ||
                !readBinary(reader, result.agent.maximum_slope_degrees) ||
                !readBinary(reader, result.horizontal_resolution) ||
                !readBinary(reader, result.vertical_resolution) ||
                !readBinary(reader, area_count) || !readBinary(reader, portal_count) ||
                magic != kRegionMagic ||
                version != kNavigationRegion3DSchemaVersion ||
                result.region != blob.region || area_count == 0u ||
                area_count > kMaximumAreas ||
                portal_count > kMaximumPortals)
            {
                return lux::cxx::unexpected(
                    fail(ENavigationRegion3DError::INVALID_CONTENT,
                         "navigation region header is invalid"));
            }
            result.areas.reserve(area_count);
            for (std::uint32_t area_index = 0u; area_index < area_count;
                 ++area_index)
            {
                std::uint8_t point_count = 0u;
                DecodedArea area;
                if (!readBinary(reader, point_count) ||
                    !readBinary(reader, area.area_class) ||
                    !readBinary(reader, area.traversal_flags) || point_count < 3u ||
                    point_count > kMaximumBoundaryPoints)
                {
                    return lux::cxx::unexpected(
                        fail(ENavigationRegion3DError::INVALID_CONTENT,
                             "navigation area header is invalid"));
                }
                area.boundary.resize(point_count);
                for (auto& point : area.boundary)
                {
                    if (!readBinary(reader, point.x) || !readBinary(reader, point.y) ||
                        !readBinary(reader, point.z))
                    {
                        return lux::cxx::unexpected(
                            fail(ENavigationRegion3DError::INVALID_CONTENT,
                                 "navigation area boundary is truncated"));
                    }
                }
                result.areas.push_back(std::move(area));
            }
            result.portals.reserve(portal_count);
            for (std::uint32_t portal_index = 0u;
                 portal_index < portal_count; ++portal_index)
            {
                NavigationPortal portal;
                std::uint8_t bidirectional = 0u;
                if (!readBinary(reader, portal.id.high) ||
                    !readBinary(reader, portal.id.low) ||
                    !readBinary(reader, portal.first_region.high) ||
                    !readBinary(reader, portal.first_region.low) ||
                    !readBinary(reader, portal.second_region.high) ||
                    !readBinary(reader, portal.second_region.low) ||
                    !readBinary(reader, portal.first_position.x) ||
                    !readBinary(reader, portal.first_position.y) ||
                    !readBinary(reader, portal.first_position.z) ||
                    !readBinary(reader, portal.second_position.x) ||
                    !readBinary(reader, portal.second_position.y) ||
                    !readBinary(reader, portal.second_position.z) ||
                    !readBinary(reader, portal.traversal_cost_scale) ||
                    !readBinary(reader, bidirectional) || bidirectional > 1u)
                {
                    return lux::cxx::unexpected(
                        fail(ENavigationRegion3DError::INVALID_CONTENT,
                             "navigation portal is truncated"));
                }
                portal.bidirectional = bidirectional != 0u;
                result.portals.push_back(std::move(portal));
            }
            NavigationRegion3DDescription validation;
            validation.region = result.region;
            validation.agent = result.agent;
            validation.horizontal_resolution = result.horizontal_resolution;
            validation.vertical_resolution = result.vertical_resolution;
            validation.areas.reserve(result.areas.size());
            for (const auto& area : result.areas)
            {
                validation.areas.push_back(
                    {area.boundary, area.area_class, area.traversal_flags});
            }
            validation.portals = result.portals;
            if (!reader.requireFullyConsumed() || !validDescription(validation))
            {
                return lux::cxx::unexpected(
                    fail(ENavigationRegion3DError::INVALID_CONTENT,
                         "navigation region payload failed validation"));
            }
            return result;
        }

    } // namespace

    lux::cxx::expected<NavigationRegion3DBlob, NavigationRegion3DFailure>
    encodeNavigationRegion3D(
        const NavigationRegion3DDescription& description) noexcept
    {
        if (!validDescription(description))
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST,
                     "navigation region description is invalid"));
        }
        lux::cxx::BinaryVectorWriter writer;
        writeBinary(writer, kRegionMagic);
        writeBinary(writer, kNavigationRegion3DSchemaVersion);
        writeBinary(writer, description.region.high);
        writeBinary(writer, description.region.low);
        writeBinary(writer, description.agent.radius);
        writeBinary(writer, description.agent.height);
        writeBinary(writer, description.agent.maximum_climb);
        writeBinary(writer, description.agent.maximum_slope_degrees);
        writeBinary(writer, description.horizontal_resolution);
        writeBinary(writer, description.vertical_resolution);
        const auto count = static_cast<std::uint32_t>(description.areas.size());
        writeBinary(writer, count);
        const auto portal_count =
            static_cast<std::uint32_t>(description.portals.size());
        writeBinary(writer, portal_count);
        for (const auto& area : description.areas)
        {
            const auto point_count =
                static_cast<std::uint8_t>(area.boundary.size());
            writeBinary(writer, point_count);
            writeBinary(writer, area.area_class);
            writeBinary(writer, area.traversal_flags);
            for (const auto& point : area.boundary)
            {
                writeBinary(writer, point.x);
                writeBinary(writer, point.y);
                writeBinary(writer, point.z);
            }
        }
        for (const auto& portal : description.portals)
        {
            writeBinary(writer, portal.id.high);
            writeBinary(writer, portal.id.low);
            writeBinary(writer, portal.first_region.high);
            writeBinary(writer, portal.first_region.low);
            writeBinary(writer, portal.second_region.high);
            writeBinary(writer, portal.second_region.low);
            writeBinary(writer, portal.first_position.x);
            writeBinary(writer, portal.first_position.y);
            writeBinary(writer, portal.first_position.z);
            writeBinary(writer, portal.second_position.x);
            writeBinary(writer, portal.second_position.y);
            writeBinary(writer, portal.second_position.z);
            writeBinary(writer, portal.traversal_cost_scale);
            writeBinary(writer, static_cast<std::uint8_t>(portal.bidirectional));
        }
        const auto bytes = std::move(writer).take();
        return NavigationRegion3DBlob{description.region,
                                      kNavigationRegion3DSchemaVersion,
                                      lux::cxx::SharedBytes<>::copyOf(bytes)};
    }

    lux::cxx::expected<NavigationRegion3DBlob, NavigationRegion3DFailure>
    navigationRegion3DBlobFromBytes(lux::cxx::SharedBytes<> payload) noexcept
    {
        lux::cxx::BinaryReader reader{payload.view()};
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        NavigationRegionId region;
        if (!readBinary(reader, magic) || !readBinary(reader, version) ||
            !readBinary(reader, region.high) || !readBinary(reader, region.low) ||
            magic != kRegionMagic ||
            version != kNavigationRegion3DSchemaVersion || !region.valid())
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_CONTENT,
                     "navigation region blob header is invalid"));
        }
        return NavigationRegion3DBlob{
            region, kNavigationRegion3DSchemaVersion, std::move(payload)};
    }

    lux::cxx::expected<PreparedNavigationRegion3D, NavigationRegion3DFailure>
    prepareNavigationRegion3D(NavigationRegion3DBlob blob,
                              std::uint64_t request_generation) noexcept
    {
        if (request_generation == 0u)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST,
                     "navigation preparation requires a non-zero generation"));
        }
        auto decoded = decodeRegion(blob);
        if (!decoded)
            return lux::cxx::unexpected(std::move(decoded.error()));
        lux::math::Position3d minimum =
            decoded->areas.front().boundary.front();
        auto maximum = minimum;
        for (const auto& area : decoded->areas)
        {
            for (const auto& point : area.boundary)
            {
                minimum.x = std::min(minimum.x, point.x);
                minimum.y = std::min(minimum.y, point.y);
                minimum.z = std::min(minimum.z, point.z);
                maximum.x = std::max(maximum.x, point.x);
                maximum.y = std::max(maximum.y, point.y);
                maximum.z = std::max(maximum.z, point.z);
            }
        }

        std::vector<QuantizedPoint> unique_points;
        std::unordered_map<QuantizedPoint,
                           std::uint16_t,
                           QuantizedPointHash>
            point_indices;
        std::vector<std::vector<std::uint16_t>> area_points;
        area_points.reserve(decoded->areas.size());
        for (const auto& area : decoded->areas)
        {
            std::vector<std::uint16_t> indices;
            indices.reserve(area.boundary.size());
            for (const auto& point : area.boundary)
            {
                const auto quantize =
                    [](double value,
                       double origin,
                       float resolution) -> std::optional<std::uint16_t>
                {
                    const auto scaled = std::round(
                        (value - origin) / static_cast<double>(resolution));
                    if (!std::isfinite(scaled) || scaled < 0.0 ||
                        scaled > 65'535.0)
                    {
                        return std::nullopt;
                    }
                    return static_cast<std::uint16_t>(scaled);
                };
                const auto x = quantize(
                    point.x, minimum.x, decoded->horizontal_resolution);
                const auto y =
                    quantize(point.y, minimum.y, decoded->vertical_resolution);
                const auto z = quantize(
                    point.z, minimum.z, decoded->horizontal_resolution);
                if (!x || !y || !z)
                {
                    return lux::cxx::unexpected(
                        fail(ENavigationRegion3DError::INVALID_CONTENT,
                             "navigation region exceeds quantized extent"));
                }
                const QuantizedPoint quantized{*x, *y, *z};
                const auto found = point_indices.find(quantized);
                std::uint16_t index = 0u;
                if (found == point_indices.end())
                {
                    const auto next = unique_points.size();
                    if (next >= 65'535u)
                    {
                        return lux::cxx::unexpected(
                            fail(ENavigationRegion3DError::INVALID_CONTENT,
                                 "navigation region has too many points"));
                    }
                    unique_points.push_back(quantized);
                    index = static_cast<std::uint16_t>(next);
                    point_indices.emplace(quantized, index);
                }
                else
                {
                    index = found->second;
                }
                indices.push_back(index);
            }
            area_points.push_back(std::move(indices));
        }

        constexpr std::size_t kPointsPerArea = kMaximumBoundaryPoints;
        constexpr std::size_t kNoNeighbour =
            std::numeric_limits<std::size_t>::max();
        std::vector<std::array<std::size_t, kPointsPerArea>> neighbours(
            area_points.size());
        for (auto& area : neighbours)
            area.fill(kNoNeighbour);

        // Establish deterministic adjacency in linear expected time.  Edges
        // within one storage layer use native polygon neighbours.  Edges that
        // cross a layer boundary become a bidirectional backend connection in
        // exactly one of the two layers.
        std::unordered_map<UndirectedEdge, EdgeUse, UndirectedEdgeHash>
            edge_uses;
        edge_uses.reserve(area_points.size() * kPointsPerArea);
        for (std::size_t area_index = 0u; area_index < area_points.size();
             ++area_index)
        {
            const auto& indices = area_points[area_index];
            for (std::size_t edge = 0u; edge < indices.size(); ++edge)
            {
                const auto from = indices[edge];
                const auto to = indices[(edge + 1u) % indices.size()];
                const UndirectedEdge key{std::min(from, to),
                                         std::max(from, to)};
                const auto [found, inserted] = edge_uses.emplace(
                    key, EdgeUse{area_index, edge, from, to, false});
                if (inserted)
                    continue;
                auto& first = found->second;
                if (first.paired || first.from != to || first.to != from)
                {
                    return lux::cxx::unexpected(
                        fail(ENavigationRegion3DError::INVALID_CONTENT,
                             "navigation topology contains a non-manifold "
                             "edge"));
                }
                neighbours[first.area][first.edge] = area_index;
                neighbours[area_index][edge] = first.area;
                first.paired = true;
            }
        }

        const auto layer_count =
            (decoded->areas.size() + kAreasPerLayer - 1u) /
            kAreasPerLayer;
        if (layer_count == 0u || layer_count > 16'384u)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::CAPACITY_EXHAUSTED,
                     "navigation region requires too many storage layers"));
        }
        std::vector<detail::PreparedLayer> layers;
        layers.reserve(layer_count);
        std::uint64_t owned_bytes = 0u;
        std::size_t maximum_polygons_per_layer = 1u;
        std::uint32_t connection_id = 1u;
        const auto toLocal = [&minimum](
            const lux::math::Position3d& point) noexcept
        {
            return std::array<float, 3u>{
                static_cast<float>(point.x - minimum.x),
                static_cast<float>(point.y - minimum.y),
                static_cast<float>(point.z - minimum.z)};
        };
        const auto interiorPoint = [](const DecodedArea& area,
                                      const lux::math::Position3d& midpoint,
                                      double epsilon) noexcept
        {
            lux::math::Position3d centroid{};
            for (const auto& point : area.boundary)
            {
                centroid.x += point.x;
                centroid.y += point.y;
                centroid.z += point.z;
            }
            const auto inverse = 1.0 / static_cast<double>(area.boundary.size());
            centroid.x *= inverse;
            centroid.y *= inverse;
            centroid.z *= inverse;
            const auto dx = centroid.x - midpoint.x;
            const auto dz = centroid.z - midpoint.z;
            const auto length = std::sqrt(dx * dx + dz * dz);
            if (length <= 1.0e-12)
                return midpoint;
            return lux::math::Position3d{
                midpoint.x + dx / length * epsilon,
                midpoint.y,
                midpoint.z + dz / length * epsilon};
        };

        for (std::size_t layer_index = 0u; layer_index < layer_count;
             ++layer_index)
        {
            const auto first_area = layer_index * kAreasPerLayer;
            const auto last_area = std::min(
                decoded->areas.size(), first_area + kAreasPerLayer);
            const auto area_count = last_area - first_area;
            std::unordered_map<std::uint16_t, std::uint16_t> local_indices;
            local_indices.reserve(area_count * kPointsPerArea);
            std::vector<std::uint16_t> points;
            std::vector<std::uint16_t> polygons(
                area_count * kPointsPerArea * 2u, 0xffffu);
            std::vector<std::uint8_t> area_classes;
            std::vector<std::uint16_t> flags;
            std::vector<float> connection_vertices;
            std::vector<float> connection_radii;
            std::vector<std::uint8_t> connection_directions;
            std::vector<std::uint8_t> connection_areas;
            std::vector<std::uint16_t> connection_flags;
            std::vector<std::uint32_t> connection_ids;
            area_classes.reserve(area_count);
            flags.reserve(area_count);

            for (std::size_t area_index = first_area;
                 area_index < last_area; ++area_index)
            {
                const auto local_area = area_index - first_area;
                const auto base = local_area * kPointsPerArea * 2u;
                const auto& indices = area_points[area_index];
                for (std::size_t edge = 0u; edge < indices.size(); ++edge)
                {
                    const auto global_point = indices[edge];
                    const auto [found, inserted] = local_indices.emplace(
                        global_point,
                        static_cast<std::uint16_t>(local_indices.size()));
                    if (inserted)
                    {
                        const auto point = unique_points[global_point];
                        points.push_back(point.x);
                        points.push_back(point.y);
                        points.push_back(point.z);
                    }
                    polygons[base + edge] = found->second;

                    const auto neighbour = neighbours[area_index][edge];
                    if (neighbour == kNoNeighbour)
                        continue;
                    if (neighbour >= first_area && neighbour < last_area)
                    {
                        polygons[base + kPointsPerArea + edge] =
                            static_cast<std::uint16_t>(
                                neighbour - first_area);
                        continue;
                    }
                    if (area_index > neighbour)
                        continue;

                    const auto& boundary =
                        decoded->areas[area_index].boundary;
                    const auto& first = boundary[edge];
                    const auto& second =
                        boundary[(edge + 1u) % boundary.size()];
                    const lux::math::Position3d midpoint{
                        (first.x + second.x) * 0.5,
                        (first.y + second.y) * 0.5,
                        (first.z + second.z) * 0.5};
                    const auto epsilon = std::max(
                        static_cast<double>(decoded->horizontal_resolution) *
                            0.25,
                        1.0e-4);
                    const auto source = interiorPoint(
                        decoded->areas[area_index], midpoint, epsilon);
                    const auto destination = interiorPoint(
                        decoded->areas[neighbour], midpoint, epsilon);
                    const auto local_source = toLocal(source);
                    const auto local_destination = toLocal(destination);
                    connection_vertices.insert(connection_vertices.end(),
                                               local_source.begin(),
                                               local_source.end());
                    connection_vertices.insert(connection_vertices.end(),
                                               local_destination.begin(),
                                               local_destination.end());
                    connection_radii.push_back(std::max(
                        decoded->horizontal_resolution,
                        decoded->agent.radius));
                    connection_directions.push_back(DT_OFFMESH_CON_BIDIR);
                    connection_areas.push_back(
                        decoded->areas[area_index].area_class);
                    connection_flags.push_back(
                        decoded->areas[area_index].traversal_flags);
                    connection_ids.push_back(connection_id++);
                }
                area_classes.push_back(
                    decoded->areas[area_index].area_class);
                flags.push_back(
                    decoded->areas[area_index].traversal_flags);
            }

            dtNavMeshCreateParams parameters{};
            parameters.verts = points.data();
            parameters.vertCount = static_cast<int>(points.size() / 3u);
            parameters.polys = polygons.data();
            parameters.polyFlags = flags.data();
            parameters.polyAreas = area_classes.data();
            parameters.polyCount = static_cast<int>(area_count);
            parameters.nvp = static_cast<int>(kPointsPerArea);
            parameters.offMeshConVerts = connection_vertices.empty()
                ? nullptr
                : connection_vertices.data();
            parameters.offMeshConRad = connection_radii.empty()
                ? nullptr
                : connection_radii.data();
            parameters.offMeshConDir = connection_directions.empty()
                ? nullptr
                : connection_directions.data();
            parameters.offMeshConAreas = connection_areas.empty()
                ? nullptr
                : connection_areas.data();
            parameters.offMeshConFlags = connection_flags.empty()
                ? nullptr
                : connection_flags.data();
            parameters.offMeshConUserID = connection_ids.empty()
                ? nullptr
                : connection_ids.data();
            parameters.offMeshConCount =
                static_cast<int>(connection_ids.size());
            parameters.walkableHeight = decoded->agent.height;
            parameters.walkableRadius = decoded->agent.radius;
            parameters.walkableClimb = decoded->agent.maximum_climb;
            parameters.cs = decoded->horizontal_resolution;
            parameters.ch = decoded->vertical_resolution;
            parameters.tileX = 0;
            parameters.tileY = 0;
            parameters.tileLayer = static_cast<int>(layer_index);
            parameters.bmin[0] = 0.0f;
            parameters.bmin[1] = 0.0f;
            parameters.bmin[2] = 0.0f;
            parameters.bmax[0] = std::max(
                decoded->horizontal_resolution,
                static_cast<float>(maximum.x - minimum.x));
            parameters.bmax[1] = std::max(
                decoded->vertical_resolution,
                static_cast<float>(maximum.y - minimum.y));
            parameters.bmax[2] = std::max(
                decoded->horizontal_resolution,
                static_cast<float>(maximum.z - minimum.z));
            parameters.buildBvTree = true;

            unsigned char* navigation_data = nullptr;
            int navigation_data_size = 0;
            if (!dtCreateNavMeshData(
                    &parameters, &navigation_data, &navigation_data_size) ||
                !navigation_data || navigation_data_size <= 0)
            {
                dtFree(navigation_data);
                return lux::cxx::unexpected(
                    fail(ENavigationRegion3DError::BUILD_FAILED,
                         "navigation backend rejected one storage layer"));
            }
            owned_bytes += static_cast<std::uint64_t>(navigation_data_size);
            maximum_polygons_per_layer = std::max(
                maximum_polygons_per_layer,
                area_count + connection_ids.size());
            layers.push_back(detail::PreparedLayer{
                std::unique_ptr<unsigned char, detail::NavigationDataOwner>{
                    navigation_data},
                navigation_data_size,
                0u});
        }

        auto* navigation_raw = dtAllocNavMesh();
        if (!navigation_raw)
            std::abort();
        std::unique_ptr<dtNavMesh, detail::NavigationOwner> navigation{
            navigation_raw};
        dtNavMeshParams navigation_parameters{};
        navigation_parameters.orig[0] = 0.0f;
        navigation_parameters.orig[1] = 0.0f;
        navigation_parameters.orig[2] = 0.0f;
        navigation_parameters.tileWidth = std::max(
            decoded->horizontal_resolution,
            static_cast<float>(maximum.x - minimum.x) +
                decoded->horizontal_resolution);
        navigation_parameters.tileHeight = std::max(
            decoded->horizontal_resolution,
            static_cast<float>(maximum.z - minimum.z) +
                decoded->horizontal_resolution);
        navigation_parameters.maxTiles = static_cast<int>(layer_count);
        navigation_parameters.maxPolys = static_cast<int>(
            maximum_polygons_per_layer);
        const auto navigation_status = navigation->init(
            &navigation_parameters);
        if (dtStatusFailed(navigation_status))
        {
            if (dtStatusDetail(navigation_status, DT_OUT_OF_MEMORY))
                std::abort();
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::BUILD_FAILED,
                     "navigation backend rejected prepared content"));
        }
        auto* query_raw = dtAllocNavMeshQuery();
        if (!query_raw)
            std::abort();
        std::unique_ptr<dtNavMeshQuery, detail::QueryOwner> query{query_raw};
        const auto query_status = query->init(navigation.get(), 2048);
        if (dtStatusFailed(query_status))
        {
            if (dtStatusDetail(query_status, DT_OUT_OF_MEMORY))
                std::abort();
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::BUILD_FAILED,
                     "navigation query rejected prepared content"));
        }

        auto data = std::make_shared<PreparedNavigationRegion3D::Data>();
        data->region = decoded->region;
        data->agent = decoded->agent;
        data->origin = minimum;
        data->bounds_min = minimum;
        data->bounds_max = maximum;
        data->request_generation = request_generation;
        // The immutable source allocation remains owned and accounted by
        // SectionBlobStore.  This backend owns only the Detour allocation
        // transferred to dtNavMesh here; counting the blob again would make
        // the owner ledger non-additive.
        data->owned_bytes = owned_bytes;
        data->resident_bytes = owned_bytes;
        data->navigation = std::move(navigation);
        data->query = std::move(query);
        data->layers = std::move(layers);
        data->portals = std::move(decoded->portals);
        return PreparedNavigationRegion3D{std::move(data)};
    }
} // namespace lux::navigation::detour3d
