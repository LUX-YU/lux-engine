#include <lux/engine/render/renderer/features/terrain/TerrainResources.hpp>

#include <cassert>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    std::vector<std::byte> pageBytes()
    {
        using namespace lux::render;
        std::vector<std::byte> result(
            TerrainResources::expectedPageBytes(), std::byte{});
        TerrainWirePageDataHeader header;
        header.height_count =
            kTerrainWireSampleEdge * kTerrainWireSampleEdge;
        header.weight_plane_bytes = header.height_count * 4u;
        header.hole_bytes = (header.height_count + 7u) / 8u;
        header.min_max_node_count = kTerrainWireMinMaxNodeCount;
        constexpr auto fallback_edge =
            (kTerrainWireQuadEdge / 2u) + 1u;
        header.fallback_height_count = fallback_edge * fallback_edge;
        std::memcpy(result.data(), &header, sizeof(header));
        return result;
    }
} // namespace

int main()
{
    using namespace lux::render;
    TerrainResources resources{1u};
    auto bytes = pageBytes();

    UploadTerrainPagePayload first;
    first.scene_id = RenderSceneId{0u, 1u};
    first.id.bytes[0] = 1u;
    first.revision = 1u;
    first.height_min = -100.0f;
    first.height_max = 300.0f;
    first.sample_spacing = 0.5f;
    first.origin.page_delta[0] = 20;
    assert(resources.upsert(first, bytes));
    assert(resources.find(first.id));
    assert(resources.find(first.id)->cache_slot == 0u);

    resources.beginFrame();
    auto second = first;
    second.id.bytes[0] = 2u;
    assert(resources.upsert(second, bytes));
    assert(resources.find(first.id)->cache_slot == 0xffffffffu);
    assert(resources.find(second.id)->cache_slot == 0u);
    auto stats = resources.stats();
    assert(stats.resident_pages == 2u);
    assert(stats.full_resolution_pages == 1u);
    assert(stats.fallback_pages == 1u);

    const std::int64_t origin_delta[3]{10, 0, 0};
    assert(resources.canRebaseSceneOrigin(origin_delta));
    resources.rebaseSceneOrigin(origin_delta);
    assert(resources.find(first.id)->header.origin.page_delta[0] == 10);
    assert(resources.find(second.id)->header.origin.page_delta[0] == 10);
    const std::int64_t rejected_delta[3]{
        std::numeric_limits<std::int64_t>::max(), 0, 0};
    assert(!resources.canRebaseSceneOrigin(rejected_delta));
    assert(resources.find(first.id)->header.origin.page_delta[0] == 10);

    resources.beginFrame();
    assert(resources.touch(first.id));
    assert(resources.find(first.id)->cache_slot == 0u);
    assert(resources.find(second.id)->cache_slot == 0xffffffffu);

    // A control-lane tombstone may overtake an older upload. The stale upload
    // must not resurrect the evicted Page.
    assert(resources.remove(first.id, 3u));
    assert(!resources.find(first.id));
    first.revision = 2u;
    assert(!resources.accepts(first.id, first.revision));
    assert(!resources.upsert(first, bytes));

    auto malformed = bytes;
    TerrainWirePageDataHeader invalid{};
    std::memcpy(&invalid, malformed.data(), sizeof(invalid));
    invalid.min_max_node_count = 1u;
    std::memcpy(malformed.data(), &invalid, sizeof(invalid));
    second.revision = 2u;
    assert(!resources.upsert(second, malformed));

    // The final min/max pair is the canonical pyramid root. A reversed root
    // is rejected in O(1) before any cache state is published.
    auto invalid_root = bytes;
    const auto root_offset = sizeof(TerrainWirePageDataHeader) +
        static_cast<std::size_t>(
            kTerrainWireSampleEdge * kTerrainWireSampleEdge) *
            sizeof(std::uint16_t) +
        static_cast<std::size_t>(
            kTerrainWireSampleEdge * kTerrainWireSampleEdge * 4u) * 2u +
        static_cast<std::size_t>(
            (kTerrainWireSampleEdge * kTerrainWireSampleEdge + 7u) / 8u) +
        static_cast<std::size_t>(kTerrainWireMinMaxNodeCount - 1u) *
            sizeof(std::uint16_t) * 2u;
    const std::uint16_t invalid_min = 2u;
    const std::uint16_t invalid_max = 1u;
    std::memcpy(
        invalid_root.data() + root_offset,
        &invalid_min,
        sizeof(invalid_min));
    std::memcpy(
        invalid_root.data() + root_offset + sizeof(std::uint16_t),
        &invalid_max,
        sizeof(invalid_max));
    second.revision = 3u;
    assert(!resources.upsert(second, invalid_root));

    TerrainResources hierarchy{4u};
    UploadTerrainPagePayload parent = first;
    parent.id = {};
    parent.id.bytes[0] = 10u;
    parent.origin.page_delta[0] = 0;
    parent.sample_spacing = 2.0f;
    parent.geometric_error = 100.0f;
    parent.hierarchy_level = 1u;
    parent.child_count = 2u;
    parent.children[0].bytes[0] = 11u;
    parent.children[1].bytes[0] = 12u;
    assert(hierarchy.upsert(parent, bytes));
    auto child_a = first;
    child_a.id = parent.children[0];
    child_a.parent = parent.id;
    child_a.origin.page_delta[0] = 0;
    auto child_b = first;
    child_b.id = parent.children[1];
    child_b.parent = parent.id;
    child_b.origin.local[0] = 128.0f;
    assert(hierarchy.upsert(child_a, bytes));
    assert(hierarchy.upsert(child_b, bytes));
    const TerrainResources::ViewOrigin near_view{
        RenderLargePosition3D{}, 1024.0f, 1000.0f};
    hierarchy.reconcileWanted(
        std::span{&near_view, 1u}, 4096.0f, 1.0f, 120u);
    assert(!hierarchy.find(parent.id)->drawable_target);
    assert(hierarchy.find(parent.id)->transition_active);
    assert(hierarchy.find(child_a.id)->drawable_target);
    assert(hierarchy.find(child_b.id)->drawable_target);
    hierarchy.reconcileWanted(
        std::span{&near_view, 1u}, 4096.0f, 2.0f, 120u);
    assert(!hierarchy.find(parent.id)->transition_active);
    assert(hierarchy.find(parent.id)->transition_end_coverage == 0.0f);
    assert(hierarchy.find(child_a.id)->transition_end_coverage == 1.0f);
    auto far_view = near_view;
    far_view.position.page_delta[0] = 10000;
    hierarchy.reconcileWanted(
        std::span{&far_view, 1u}, 20'000'000.0f, 3.0f, 120u);
    assert(hierarchy.find(parent.id)->drawable_target);
    assert(!hierarchy.find(child_a.id)->drawable_target);
    assert(hierarchy.stats().transition_pages == 3u);

    // A late grandchild arrival must not skip across a middle level that is
    // still fading in. That middle page is the stable continuity fallback.
    TerrainResources staged{8u};
    auto root = parent;
    root.id.bytes[0] = 20u;
    root.hierarchy_level = 2u;
    root.children[0].bytes[0] = 21u;
    root.children[1].bytes[0] = 22u;
    assert(staged.upsert(root, bytes));
    auto middle_a = parent;
    middle_a.id = root.children[0];
    middle_a.parent = root.id;
    middle_a.children[0].bytes[0] = 23u;
    middle_a.children[1].bytes[0] = 24u;
    auto middle_b = middle_a;
    middle_b.id = root.children[1];
    middle_b.children[0].bytes[0] = 25u;
    middle_b.children[1].bytes[0] = 26u;
    assert(staged.upsert(middle_a, bytes));
    assert(staged.upsert(middle_b, bytes));
    staged.reconcileWanted(
        std::span{&near_view, 1u}, 4096.0f, 10.0f, 120u);
    assert(staged.find(middle_a.id)->transition_active);

    const auto add_leaf = [&](TerrainWireId id, TerrainWireId owner)
    {
        auto leaf = first;
        leaf.id = id;
        leaf.parent = owner;
        assert(staged.upsert(leaf, bytes));
    };
    add_leaf(middle_a.children[0], middle_a.id);
    add_leaf(middle_a.children[1], middle_a.id);
    add_leaf(middle_b.children[0], middle_b.id);
    add_leaf(middle_b.children[1], middle_b.id);
    staged.reconcileWanted(
        std::span{&near_view, 1u}, 4096.0f, 10.1f, 120u);
    assert(staged.find(middle_a.id)->drawable_target);
    assert(staged.find(middle_b.id)->drawable_target);
    assert(!staged.find(middle_a.children[0])->drawable_target);
    assert(!staged.find(middle_b.children[0])->drawable_target);

    return 0;
}
