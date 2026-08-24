#include <lux/engine/authoring/world/WorldTerrainAuthoring.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lux::authoring
{
    namespace
    {
        constexpr std::uint32_t kQuadEdge = kWorldLogicalChunkEdge;
        constexpr std::uint32_t kSampleEdge = kWorldTerrainSampleEdge;
        constexpr std::uint32_t kSampleCount = kSampleEdge * kSampleEdge;
        constexpr std::uint32_t kWeightBytes = kSampleCount * 4u;
        constexpr std::uint32_t kHoleBytes = (kSampleCount + 7u) / 8u;
        constexpr float kSeamEpsilon = 1.0e-5f;

        struct SampleKey final
        {
            std::int64_t cell_a{0};
            std::int64_t cell_b{0};
            std::uint16_t local_x{0u};
            std::uint16_t local_y{0u};

            friend bool operator==(const SampleKey&, const SampleKey&) =
                default;
        };

        struct SampleKeyHash final
        {
            std::size_t operator()(const SampleKey& key) const noexcept
            {
                auto value = static_cast<std::uint64_t>(key.cell_a);
                value ^= static_cast<std::uint64_t>(key.cell_b) +
                    0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
                value ^= static_cast<std::uint64_t>(key.local_x) << 16u;
                value ^= static_cast<std::uint64_t>(key.local_y);
                return static_cast<std::size_t>(value);
            }
        };

        struct PageView final
        {
            const WorldTerrainPageDocument* page{nullptr};
            lux::authoring::PlanarCellCoord cell;
        };

        struct SampleLocation final
        {
            std::size_t page{0u};
            std::uint32_t sample{0u};
        };

        using LocationMap = std::unordered_map<
            SampleKey,
            std::vector<SampleLocation>,
            SampleKeyHash>;

        [[nodiscard]] WorldTerrainAuthoringFailure failure(
            EWorldTerrainAuthoringError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        [[nodiscard]] SampleKey sampleKey(
            lux::authoring::PlanarCellCoord cell,
            std::uint32_t x,
            std::uint32_t y) noexcept
        {
            if (x == kQuadEdge)
            {
                ++cell.a;
                x = 0u;
            }
            if (y == kQuadEdge)
            {
                ++cell.b;
                y = 0u;
            }
            return {
                cell.a,
                cell.b,
                static_cast<std::uint16_t>(x),
                static_cast<std::uint16_t>(y)};
        }

        [[nodiscard]] SampleKey neighbour(
            SampleKey key,
            std::int32_t dx,
            std::int32_t dy) noexcept
        {
            auto x = static_cast<std::int32_t>(key.local_x) + dx;
            auto y = static_cast<std::int32_t>(key.local_y) + dy;
            if (x < 0)
            {
                --key.cell_a;
                x += static_cast<std::int32_t>(kQuadEdge);
            }
            else if (x >= static_cast<std::int32_t>(kQuadEdge))
            {
                ++key.cell_a;
                x -= static_cast<std::int32_t>(kQuadEdge);
            }
            if (y < 0)
            {
                --key.cell_b;
                y += static_cast<std::int32_t>(kQuadEdge);
            }
            else if (y >= static_cast<std::int32_t>(kQuadEdge))
            {
                ++key.cell_b;
                y -= static_cast<std::int32_t>(kQuadEdge);
            }
            key.local_x = static_cast<std::uint16_t>(x);
            key.local_y = static_cast<std::uint16_t>(y);
            return key;
        }

        [[nodiscard]] bool bitAt(
            const std::vector<std::uint8_t>& bytes,
            std::uint32_t index) noexcept
        {
            return (bytes[index / 8u] & (1u << (index % 8u))) != 0u;
        }

        void setBit(
            std::vector<std::uint8_t>& bytes,
            std::uint32_t index,
            bool value) noexcept
        {
            const auto mask = static_cast<std::uint8_t>(
                1u << (index % 8u));
            auto& byte = bytes[index / 8u];
            byte = value
                ? static_cast<std::uint8_t>(byte | mask)
                : static_cast<std::uint8_t>(byte & ~mask);
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<PageView>,
            WorldTerrainAuthoringFailure>
        validatePages(
            const WorldSourceDocument& root,
            std::span<const WorldTerrainPageDocument> pages,
            bool require_rectangular)
        {
            if (pages.empty())
            {
                return lux::cxx::unexpected(failure(
                    EWorldTerrainAuthoringError::INVALID_ARGUMENT,
                    "Terrain editing requires a non-empty PLANAR_XZ region"));
            }
            const auto& first = pages.front();
            std::vector<PageView> result;
            result.reserve(pages.size());
            std::unordered_set<SampleKey, SampleKeyHash> cells;
            for (const auto& page : pages)
            {
                if (page.world != root.world || page.world != first.world ||
                    page.terrain_set != first.terrain_set ||
                    page.space != first.space)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldTerrainAuthoringError::MIXED_TERRAIN_SET,
                        "Terrain transaction mixes World, Space or Terrain Set"));
                }
                if (page.cell.topology !=
                        lux::authoring::EPartitionTopology::PLANAR_XZ ||
                    !std::holds_alternative<lux::authoring::PlanarCellCoord>(
                        page.cell.coordinate) ||
                    !std::isfinite(page.height_min) ||
                    !std::isfinite(page.height_max) ||
                    !(page.height_max > page.height_min) ||
                    !std::isfinite(page.sample_spacing) ||
                    !(page.sample_spacing > 0.0f) ||
                    page.heights.size() != kSampleCount ||
                    page.weight_layer_count > 8u ||
                    page.weight_planes[0].size() != kWeightBytes ||
                    page.weight_planes[1].size() != kWeightBytes ||
                    page.holes.size() != kHoleBytes)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldTerrainAuthoringError::INVALID_PAGE,
                        "Terrain transaction contains an invalid LXTP page"));
                }
                if (page.height_min != first.height_min ||
                    page.height_max != first.height_max ||
                    page.sample_spacing != first.sample_spacing)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldTerrainAuthoringError::MIXED_TERRAIN_SET,
                        "Terrain pages disagree on global range or spacing"));
                }
                const auto cell = std::get<lux::authoring::PlanarCellCoord>(
                    page.cell.coordinate);
                const SampleKey cell_key{cell.a, cell.b, 0u, 0u};
                if (!cells.insert(cell_key).second)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldTerrainAuthoringError::INVALID_PAGE,
                        "Terrain transaction contains a duplicate Cell"));
                }
                result.push_back({&page, cell});
            }
            if (require_rectangular)
            {
                const auto [min_a, max_a] = std::ranges::minmax(
                    result, {}, [](const PageView& page)
                    {
                        return page.cell.a;
                    });
                const auto [min_b, max_b] = std::ranges::minmax(
                    result, {}, [](const PageView& page)
                    {
                        return page.cell.b;
                    });
                const auto width = static_cast<unsigned long long>(
                    max_a.cell.a - min_a.cell.a) + 1ull;
                const auto height = static_cast<unsigned long long>(
                    max_b.cell.b - min_b.cell.b) + 1ull;
                if (width > std::numeric_limits<std::uint32_t>::max() ||
                    height > std::numeric_limits<std::uint32_t>::max() ||
                    width * height != result.size())
                {
                    return lux::cxx::unexpected(failure(
                        EWorldTerrainAuthoringError::INCOMPLETE_REGION,
                        "Terrain pages do not form a complete rectangle"));
                }
            }
            return result;
        }

        [[nodiscard]] lux::cxx::expected<
            LocationMap,
            WorldTerrainAuthoringFailure>
        buildLocations(
            std::span<const PageView> pages,
            bool validate_all_channels)
        {
            LocationMap locations;
            locations.reserve(pages.size() * kSampleCount);
            for (std::size_t page_index = 0u;
                 page_index < pages.size();
                 ++page_index)
            {
                const auto& view = pages[page_index];
                for (std::uint32_t y = 0u; y < kSampleEdge; ++y)
                for (std::uint32_t x = 0u; x < kSampleEdge; ++x)
                {
                    const auto sample = y * kSampleEdge + x;
                    auto& duplicates = locations[sampleKey(view.cell, x, y)];
                    if (!duplicates.empty())
                    {
                        const auto& first = duplicates.front();
                        const auto& left = *pages[first.page].page;
                        const auto& right = *view.page;
                        if (std::fabs(left.heights[first.sample] -
                                right.heights[sample]) > kSeamEpsilon)
                        {
                            return lux::cxx::unexpected(failure(
                                EWorldTerrainAuthoringError::SEAM_MISMATCH,
                                "Terrain height seam is inconsistent"));
                        }
                        if (validate_all_channels)
                        {
                            for (std::uint32_t plane = 0u; plane < 2u; ++plane)
                            for (std::uint32_t channel = 0u; channel < 4u;
                                 ++channel)
                            {
                                if (left.weight_planes[plane][
                                        first.sample * 4u + channel] !=
                                    right.weight_planes[plane][
                                        sample * 4u + channel])
                                {
                                    return lux::cxx::unexpected(failure(
                                        EWorldTerrainAuthoringError::SEAM_MISMATCH,
                                        "Terrain weight seam is inconsistent"));
                                }
                            }
                            if (bitAt(left.holes, first.sample) !=
                                bitAt(right.holes, sample))
                            {
                                return lux::cxx::unexpected(failure(
                                    EWorldTerrainAuthoringError::SEAM_MISMATCH,
                                    "Terrain hole seam is inconsistent"));
                            }
                        }
                    }
                    duplicates.push_back({page_index, sample});
                }
            }
            return locations;
        }

        [[nodiscard]] float brushInfluence(
            float distance,
            const WorldTerrainBrush& brush) noexcept
        {
            const auto normalized = std::clamp(
                distance / brush.radius, 0.0f, 1.0f);
            if (brush.falloff <= 0.0f)
                return normalized <= 1.0f ? 1.0f : 0.0f;
            const auto hard_radius = 1.0f - brush.falloff;
            if (normalized <= hard_radius)
                return 1.0f;
            return std::clamp(
                (1.0f - normalized) / brush.falloff, 0.0f, 1.0f);
        }

        [[nodiscard]] std::optional<std::pair<long double, long double>>
        centerWithinCell(
            const lux::math::Position3d& center,
            const lux::authoring::PlanarCellCoord& cell,
            float cell_edge) noexcept
        {
            const auto local_x = static_cast<long double>(center.x) -
                static_cast<long double>(cell.a) * cell_edge;
            const auto local_z = static_cast<long double>(center.z) -
                static_cast<long double>(cell.b) * cell_edge;
            if (!std::isfinite(static_cast<double>(local_x)) ||
                !std::isfinite(static_cast<double>(local_z)))
            {
                return std::nullopt;
            }
            return std::pair{local_x, local_z};
        }

        [[nodiscard]] std::pair<lux::authoring::PlanarCellCoord,
            lux::authoring::PlanarCellCoord>
        bounds(std::span<const PageView> pages)
        {
            auto minimum = pages.front().cell;
            auto maximum = pages.front().cell;
            for (const auto& page : pages)
            {
                minimum.a = std::min(minimum.a, page.cell.a);
                minimum.b = std::min(minimum.b, page.cell.b);
                maximum.a = std::max(maximum.a, page.cell.a);
                maximum.b = std::max(maximum.b, page.cell.b);
            }
            return {minimum, maximum};
        }
    } // namespace

    lux::cxx::expected<
        WorldTerrainEditTransaction,
        WorldTerrainAuthoringFailure>
    applyWorldTerrainBrush(
        const WorldSourceDocument& root,
        std::span<const WorldTerrainPageDocument> source_pages,
        const lux::math::Position3d& center,
        const WorldTerrainBrush& brush)
    {
        if (!std::isfinite(brush.radius) || !(brush.radius > 0.0f) ||
            !std::isfinite(brush.falloff) || brush.falloff < 0.0f ||
            brush.falloff > 1.0f || !std::isfinite(brush.strength) ||
            brush.weight_layer >= 8u)
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::INVALID_ARGUMENT,
                "Terrain brush parameters are invalid"));
        }
        auto checked = validatePages(root, source_pages, false);
        if (!checked)
            return lux::cxx::unexpected(checked.error());
        const auto space = std::ranges::find(
            root.spaces,
            source_pages.front().space,
            &lux::authoring::PartitionSpaceDescriptor::id);
        if (space == root.spaces.end() || space->topology !=
                lux::authoring::EPartitionTopology::PLANAR_XZ ||
            std::fabs(space->cell_edge - source_pages.front().sample_spacing *
                    kQuadEdge) > kSeamEpsilon)
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::INVALID_PAGE,
                "Terrain Page spacing does not match its PLANAR_XZ Cell"));
        }
        if (!lux::math::isFinite(center))
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::INVALID_ARGUMENT,
                "Terrain brush center cannot be mapped to a Cell"));
        }
        const auto cell_x = std::floor(
            center.x / static_cast<double>(space->cell_edge));
        const auto cell_z = std::floor(
            center.z / static_cast<double>(space->cell_edge));
        if (cell_x < static_cast<double>(
                std::numeric_limits<std::int64_t>::min()) ||
            cell_x > static_cast<double>(
                std::numeric_limits<std::int64_t>::max()) ||
            cell_z < static_cast<double>(
                std::numeric_limits<std::int64_t>::min()) ||
            cell_z > static_cast<double>(
                std::numeric_limits<std::int64_t>::max()))
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::INVALID_ARGUMENT,
                "Terrain brush center cannot be mapped to a Cell"));
        }
        const lux::authoring::PlanarCellCoord center_cell{
            static_cast<std::int64_t>(cell_x),
            static_cast<std::int64_t>(cell_z)};
        const auto center_local = centerWithinCell(
            center, center_cell, space->cell_edge);
        if (!center_local)
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::INVALID_ARGUMENT,
                "Terrain brush center is outside supported coordinate range"));
        }
        auto locations = buildLocations(*checked, true);
        if (!locations)
            return lux::cxx::unexpected(locations.error());

        std::unordered_map<SampleKey, float, SampleKeyHash> source_heights;
        source_heights.reserve(locations->size());
        for (const auto& [key, samples] : *locations)
        {
            const auto& location = samples.front();
            source_heights.emplace(
                key,
                source_pages[location.page].heights[location.sample]);
        }

        std::vector<WorldTerrainPageDocument> edited(
            source_pages.begin(), source_pages.end());
        std::unordered_set<std::size_t> touched;
        for (const auto& [key, samples] : *locations)
        {
            const auto relative_x =
                (static_cast<long double>(key.cell_a) - center_cell.a) *
                    space->cell_edge +
                static_cast<long double>(key.local_x) *
                    source_pages.front().sample_spacing -
                center_local->first;
            const auto relative_z =
                (static_cast<long double>(key.cell_b) - center_cell.b) *
                    space->cell_edge +
                static_cast<long double>(key.local_y) *
                    source_pages.front().sample_spacing -
                center_local->second;
            const auto distance = std::sqrt(
                relative_x * relative_x + relative_z * relative_z);
            if (distance > brush.radius)
                continue;
            const auto influence = brushInfluence(
                static_cast<float>(distance), brush);
            if (!(influence > 0.0f))
                continue;

            float height = source_heights.at(key);
            if (brush.mode == EWorldTerrainBrushMode::RAISE_LOWER)
            {
                height += brush.strength * influence;
            }
            else if (brush.mode == EWorldTerrainBrushMode::FLATTEN)
            {
                height += (brush.flatten_height - height) *
                    std::clamp(std::fabs(brush.strength) * influence,
                        0.0f, 1.0f);
            }
            else if (brush.mode == EWorldTerrainBrushMode::SMOOTH)
            {
                float sum = height;
                std::uint32_t count = 1u;
                constexpr std::array<std::pair<std::int32_t, std::int32_t>, 8>
                    offsets{{
                        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                        {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};
                for (const auto [dx, dy] : offsets)
                {
                    const auto found = source_heights.find(
                        neighbour(key, dx, dy));
                    if (found != source_heights.end())
                    {
                        sum += found->second;
                        ++count;
                    }
                }
                const auto average = sum / static_cast<float>(count);
                height += (average - height) *
                    std::clamp(std::fabs(brush.strength) * influence,
                        0.0f, 1.0f);
            }
            height = std::clamp(
                height,
                source_pages.front().height_min,
                source_pages.front().height_max);

            for (const auto& location : samples)
            {
                auto& page = edited[location.page];
                if (brush.mode == EWorldTerrainBrushMode::WEIGHT_PAINT)
                {
                    const auto plane = brush.weight_layer / 4u;
                    const auto channel = brush.weight_layer % 4u;
                    auto& value = page.weight_planes[plane][
                        location.sample * 4u + channel];
                    const auto blended = static_cast<float>(value) +
                        (static_cast<float>(brush.weight_value) - value) *
                        std::clamp(std::fabs(brush.strength) * influence,
                            0.0f, 1.0f);
                    value = static_cast<std::uint8_t>(
                        std::clamp(std::lround(blended), 0l, 255l));
                    page.weight_layer_count = std::max<std::uint8_t>(
                        page.weight_layer_count,
                        static_cast<std::uint8_t>(brush.weight_layer + 1u));
                }
                else if (brush.mode == EWorldTerrainBrushMode::HOLE_PAINT)
                {
                    setBit(page.holes, location.sample, brush.hole_value);
                }
                else
                {
                    page.heights[location.sample] = height;
                }
                touched.insert(location.page);
            }
        }
        if (touched.empty())
            return WorldTerrainEditTransaction{};

        if (brush.require_complete_neighbourhood)
        {
            const auto [minimum, maximum] = bounds(*checked);
            for (const auto page_index : touched)
            {
                const auto cell = (*checked)[page_index].cell;
                const auto touches_left = cell.a == minimum.a;
                const auto touches_right = cell.a == maximum.a;
                const auto touches_bottom = cell.b == minimum.b;
                const auto touches_top = cell.b == maximum.b;
                const auto page_origin_x =
                    (static_cast<long double>(cell.a) - center_cell.a) *
                        space->cell_edge - center_local->first;
                const auto page_origin_z =
                    (static_cast<long double>(cell.b) - center_cell.b) *
                        space->cell_edge - center_local->second;
                if ((touches_left && std::fabs(page_origin_x) <= brush.radius) ||
                    (touches_right && std::fabs(
                        page_origin_x + space->cell_edge) <= brush.radius) ||
                    (touches_bottom && std::fabs(page_origin_z) <= brush.radius) ||
                    (touches_top && std::fabs(
                        page_origin_z + space->cell_edge) <= brush.radius))
                {
                    return lux::cxx::unexpected(failure(
                        EWorldTerrainAuthoringError::INCOMPLETE_REGION,
                        "Terrain brush intersects an unloaded neighbour Page"));
                }
            }
        }

        WorldTerrainEditTransaction transaction;
        transaction.before_pages.reserve(touched.size());
        transaction.after_pages.reserve(touched.size());
        std::vector<std::size_t> ordered(touched.begin(), touched.end());
        std::ranges::sort(ordered);
        for (const auto index : ordered)
        {
            transaction.before_pages.push_back(source_pages[index]);
            transaction.after_pages.push_back(std::move(edited[index]));
        }
        return transaction;
    }

    lux::cxx::expected<
        std::vector<std::byte>,
        WorldTerrainAuthoringFailure>
    encodeWorldTerrainRaw16(const WorldTerrainHeightmap16& image)
    {
        constexpr std::uint64_t maximum_samples =
            512ull * 1024ull * 1024ull;
        const auto count = static_cast<std::uint64_t>(image.width) *
            image.height;
        if (image.width == 0u || image.height == 0u ||
            count > maximum_samples || image.samples.size() != count ||
            !std::isfinite(image.height_min) ||
            !std::isfinite(image.height_max) ||
            !(image.height_max > image.height_min))
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::IMAGE_DIMENSION_MISMATCH,
                "RAW16 image has invalid dimensions or height range"));
        }
        std::vector<std::byte> bytes(
            static_cast<std::size_t>(count * 2u));
        for (std::size_t index = 0u; index < image.samples.size(); ++index)
        {
            bytes[index * 2u] = static_cast<std::byte>(
                image.samples[index] & 0xffu);
            bytes[index * 2u + 1u] = static_cast<std::byte>(
                image.samples[index] >> 8u);
        }
        return bytes;
    }

    lux::cxx::expected<
        WorldTerrainHeightmap16,
        WorldTerrainAuthoringFailure>
    decodeWorldTerrainRaw16(
        std::span<const std::byte> bytes,
        std::uint32_t width,
        std::uint32_t height,
        float height_min,
        float height_max)
    {
        constexpr std::uint64_t maximum_samples =
            512ull * 1024ull * 1024ull;
        const auto count = static_cast<std::uint64_t>(width) * height;
        if (width == 0u || height == 0u || count > maximum_samples ||
            bytes.size() != count * 2u || !std::isfinite(height_min) ||
            !std::isfinite(height_max) || !(height_max > height_min))
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::IMAGE_DIMENSION_MISMATCH,
                "RAW16 bytes do not exactly match the requested image"));
        }
        WorldTerrainHeightmap16 image;
        image.width = width;
        image.height = height;
        image.height_min = height_min;
        image.height_max = height_max;
        image.samples.resize(static_cast<std::size_t>(count));
        for (std::size_t index = 0u; index < image.samples.size(); ++index)
        {
            image.samples[index] =
                static_cast<std::uint16_t>(bytes[index * 2u]) |
                static_cast<std::uint16_t>(bytes[index * 2u + 1u]) << 8u;
        }
        return image;
    }

    lux::cxx::expected<
        WorldTerrainHeightmap16,
        WorldTerrainAuthoringFailure>
    exportWorldTerrainHeightmap16(
        const WorldSourceDocument& root,
        std::span<const WorldTerrainPageDocument> source_pages)
    {
        auto checked = validatePages(root, source_pages, true);
        if (!checked)
            return lux::cxx::unexpected(checked.error());
        auto locations = buildLocations(*checked, false);
        if (!locations)
            return lux::cxx::unexpected(locations.error());
        const auto [minimum, maximum] = bounds(*checked);
        const auto page_width = static_cast<std::uint64_t>(
            maximum.a - minimum.a + 1);
        const auto page_height = static_cast<std::uint64_t>(
            maximum.b - minimum.b + 1);
        const auto width64 = page_width * kQuadEdge + 1u;
        const auto height64 = page_height * kQuadEdge + 1u;
        if (width64 > std::numeric_limits<std::uint32_t>::max() ||
            height64 > std::numeric_limits<std::uint32_t>::max() ||
            width64 * height64 > std::numeric_limits<std::size_t>::max())
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::INVALID_ARGUMENT,
                "Terrain heightmap dimensions overflow"));
        }
        WorldTerrainHeightmap16 image;
        image.width = static_cast<std::uint32_t>(width64);
        image.height = static_cast<std::uint32_t>(height64);
        image.height_min = source_pages.front().height_min;
        image.height_max = source_pages.front().height_max;
        image.samples.resize(static_cast<std::size_t>(width64 * height64));
        const auto extent = image.height_max - image.height_min;
        for (const auto& [key, samples] : *locations)
        {
            const auto x = static_cast<std::uint64_t>(
                    key.cell_a - minimum.a) * kQuadEdge + key.local_x;
            const auto y = static_cast<std::uint64_t>(
                    key.cell_b - minimum.b) * kQuadEdge + key.local_y;
            const auto& location = samples.front();
            const auto value = std::clamp(
                (source_pages[location.page].heights[location.sample] -
                    image.height_min) / extent,
                0.0f,
                1.0f);
            image.samples[static_cast<std::size_t>(
                y * image.width + x)] = static_cast<std::uint16_t>(
                    std::lround(value * 65535.0f));
        }
        return image;
    }

    lux::cxx::expected<
        WorldTerrainEditTransaction,
        WorldTerrainAuthoringFailure>
    importWorldTerrainHeightmap16(
        const WorldSourceDocument& root,
        std::span<const WorldTerrainPageDocument> source_pages,
        const WorldTerrainHeightmap16& image)
    {
        auto checked = validatePages(root, source_pages, true);
        if (!checked)
            return lux::cxx::unexpected(checked.error());
        const auto [minimum, maximum] = bounds(*checked);
        const auto expected_width = static_cast<std::uint64_t>(
                maximum.a - minimum.a + 1) * kQuadEdge + 1u;
        const auto expected_height = static_cast<std::uint64_t>(
                maximum.b - minimum.b + 1) * kQuadEdge + 1u;
        if (image.width != expected_width || image.height != expected_height ||
            image.samples.size() != expected_width * expected_height ||
            image.height_min != source_pages.front().height_min ||
            image.height_max != source_pages.front().height_max)
        {
            return lux::cxx::unexpected(failure(
                EWorldTerrainAuthoringError::IMAGE_DIMENSION_MISMATCH,
                "16-bit heightmap dimensions or global range do not match Pages"));
        }
        auto edited = std::vector<WorldTerrainPageDocument>(
            source_pages.begin(), source_pages.end());
        const auto extent = image.height_max - image.height_min;
        for (std::size_t page_index = 0u;
             page_index < checked->size();
             ++page_index)
        {
            const auto cell = (*checked)[page_index].cell;
            const auto base_x = static_cast<std::uint64_t>(
                cell.a - minimum.a) * kQuadEdge;
            const auto base_y = static_cast<std::uint64_t>(
                cell.b - minimum.b) * kQuadEdge;
            for (std::uint32_t y = 0u; y < kSampleEdge; ++y)
            for (std::uint32_t x = 0u; x < kSampleEdge; ++x)
            {
                const auto value = image.samples[static_cast<std::size_t>(
                    (base_y + y) * image.width + base_x + x)];
                edited[page_index].heights[y * kSampleEdge + x] =
                    image.height_min + extent *
                        (static_cast<float>(value) / 65535.0f);
            }
        }
        return WorldTerrainEditTransaction{
            std::vector<WorldTerrainPageDocument>(
                source_pages.begin(), source_pages.end()),
            std::move(edited)};
    }
} // namespace lux::authoring
