#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/authoring/world/WorldTerrainAuthoring.hpp>

#include <cassert>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    [[nodiscard]] lux::authoring::WorldTerrainPageDocument makePage(
        const lux::authoring::WorldSourceDocument& root,
        std::int64_t cell_x)
    {
        using namespace lux::authoring;
        constexpr auto sample_edge = kWorldTerrainSampleEdge;
        constexpr auto sample_count = static_cast<std::size_t>(sample_edge) *
            sample_edge;
        WorldTerrainPageDocument page;
        page.world = root.world;
        page.terrain_set = lux::authoring::TerrainSetId{
            uuid("aa000000-0000-4000-8000-000000000001")};
        page.space = root.spaces.front().id;
        page.cell = {
            lux::authoring::EPartitionTopology::PLANAR_XZ,
            lux::authoring::PlanarCellCoord{cell_x, 0}};
        page.height_min = -100.0f;
        page.height_max = 300.0f;
        page.sample_spacing = 0.5f;
        page.heights.resize(sample_count);
        page.weight_planes[0].assign(sample_count * 4u, 0u);
        page.weight_planes[1].assign(sample_count * 4u, 0u);
        page.holes.assign((sample_count + 7u) / 8u, 0u);
        for (std::uint32_t y = 0u; y < sample_edge; ++y)
        for (std::uint32_t x = 0u; x < sample_edge; ++x)
        {
            page.heights[y * sample_edge + x] =
                static_cast<float>(cell_x * 256 + x) * 0.01f;
        }
        return page;
    }

    [[nodiscard]] bool holeAt(
        const lux::authoring::WorldTerrainPageDocument& page,
        std::uint32_t sample)
    {
        return (page.holes[sample / 8u] &
            (1u << (sample % 8u))) != 0u;
    }
} // namespace

int main()
{
    using namespace lux::authoring;
    constexpr auto edge = kWorldTerrainSampleEdge;
    const auto root = makeWorldSourceDocument(
        lux::authoring::EPartitionTopology::PLANAR_XZ);
    std::vector pages{makePage(root, 0), makePage(root, 1)};
    const lux::math::Position3d center{128.0, 0.0, 64.0};

    WorldTerrainBrush raise;
    raise.mode = EWorldTerrainBrushMode::RAISE_LOWER;
    raise.radius = 2.0f;
    raise.falloff = 1.0f;
    raise.strength = 4.0f;
    const auto raised = applyWorldTerrainBrush(root, pages, center, raise);
    assert(raised);
    assert(raised->after_pages.size() == 2u);
    const auto left_seam = 128u * edge + 256u;
    const auto right_seam = 128u * edge;
    assert(raised->after_pages[0].heights[left_seam] >
        raised->before_pages[0].heights[left_seam]);
    assert(std::fabs(
        raised->after_pages[0].heights[left_seam] -
        raised->after_pages[1].heights[right_seam]) < 1.0e-5f);

    WorldTerrainBrush weight = raise;
    weight.mode = EWorldTerrainBrushMode::WEIGHT_PAINT;
    weight.strength = 1.0f;
    weight.weight_layer = 5u;
    weight.weight_value = 201u;
    const auto weighted = applyWorldTerrainBrush(
        root, raised->after_pages, center, weight);
    assert(weighted);
    assert(weighted->after_pages[0].weight_layer_count == 6u);
    assert(weighted->after_pages[0].weight_planes[1][
        left_seam * 4u + 1u] == 201u);
    assert(weighted->after_pages[1].weight_planes[1][
        right_seam * 4u + 1u] == 201u);

    WorldTerrainBrush hole = raise;
    hole.mode = EWorldTerrainBrushMode::HOLE_PAINT;
    hole.hole_value = true;
    const auto holed = applyWorldTerrainBrush(
        root, weighted->after_pages, center, hole);
    assert(holed);
    assert(holeAt(holed->after_pages[0], left_seam));
    assert(holeAt(holed->after_pages[1], right_seam));

    const auto exported = exportWorldTerrainHeightmap16(
        root, holed->after_pages);
    assert(exported);
    assert(exported->width == 513u);
    assert(exported->height == 257u);
    const auto raw16 = encodeWorldTerrainRaw16(*exported);
    assert(raw16);
    assert((*raw16)[0] == static_cast<std::byte>(
        exported->samples[0] & 0xffu));
    assert((*raw16)[1] == static_cast<std::byte>(
        exported->samples[0] >> 8u));
    const auto decoded_raw16 = decodeWorldTerrainRaw16(
        *raw16,
        exported->width,
        exported->height,
        exported->height_min,
        exported->height_max);
    assert(decoded_raw16);
    assert(decoded_raw16->samples == exported->samples);
    const auto truncated_raw16 = decodeWorldTerrainRaw16(
        std::span<const std::byte>{raw16->data(), raw16->size() - 1u},
        exported->width,
        exported->height,
        exported->height_min,
        exported->height_max);
    assert(!truncated_raw16);
    auto image = *exported;
    image.samples[128u * image.width + 256u] = 65535u;
    const auto imported = importWorldTerrainHeightmap16(
        root, holed->after_pages, image);
    assert(imported);
    assert(imported->after_pages[0].heights[left_seam] == 300.0f);
    assert(imported->after_pages[1].heights[right_seam] == 300.0f);

    const std::array only_left{pages.front()};
    const auto incomplete = applyWorldTerrainBrush(
        root, only_left, center, raise);
    assert(!incomplete);
    assert(incomplete.error().error ==
        EWorldTerrainAuthoringError::INCOMPLETE_REGION);

    auto broken = pages;
    broken[1].heights[right_seam] += 1.0f;
    const auto seam_failure = exportWorldTerrainHeightmap16(root, broken);
    assert(!seam_failure);
    assert(seam_failure.error().error ==
        EWorldTerrainAuthoringError::SEAM_MISMATCH);
    return 0;
}
