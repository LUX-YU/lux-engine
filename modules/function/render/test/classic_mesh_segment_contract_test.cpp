#include <lux/engine/render/gpu/memory/ChainedArenaAllocator.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <lux/engine/render/resources/mesh/MeshSectionTable.hpp>

#include <cstdio>

namespace
{
    bool expect(bool condition, const char* message)
    {
        if (condition)
            return true;
        std::fprintf(stderr, "classic mesh segment contract: %s\n", message);
        return false;
    }
}

int main()
{
    using namespace lux::render;
    bool ok = true;

    MeshSectionTable sections;
    sections.init(nullptr, 8u);
    const MeshSectionRecord record{
        .first_index = 0u,
        .index_count = 3u,
        .base_vertex = 0,
        .vertex_count = 3u};
    const auto s0_u32 = sections.registerSection(
        record, 0u, VK_INDEX_TYPE_UINT32);
    const auto s1_u32 = sections.registerSection(
        record, 1u, VK_INDEX_TYPE_UINT32);
    const auto s1_u16 = sections.registerSection(
        record, 1u, VK_INDEX_TYPE_UINT16);
    const auto s0_u32_again = sections.registerSection(
        record, 0u, VK_INDEX_TYPE_UINT32);
    ok &= expect(s0_u32 == s0_u32_again,
        "identical segment/type section is deduplicated");
    ok &= expect(s0_u32 != s1_u32 && s1_u32 != s1_u16,
        "segment and index type are part of section identity");

    MdcTable mdcs;
    const auto m0 = mdcs.registerInstance(
        0u, 7u, s0_u32, 0u, VK_INDEX_TYPE_UINT32);
    const auto m1 = mdcs.registerInstance(
        0u, 7u, s1_u32, 1u, VK_INDEX_TYPE_UINT32);
    const auto m2 = mdcs.registerInstance(
        0u, 7u, s1_u16, 1u, VK_INDEX_TYPE_UINT16);
    ok &= expect(m0 != m1 && m1 != m2 && mdcs.count() == 3u,
        "MDC identity separates IBO segment and index type");
    ok &= expect(mdcs.entries()[m1].ibo_segment == 1u &&
            mdcs.entries()[m1].index_type == VK_INDEX_TYPE_UINT32 &&
            mdcs.entries()[m2].index_type == VK_INDEX_TYPE_UINT16,
        "draw lanes retain the exact Vulkan index binding identity");

    ChainedArenaAllocator arena{1024u, 4u};
    ok &= expect(arena.addSegment(2048u) == 1u,
        "transaction can append a growth segment");
    ok &= expect(arena.removeLastEmptySegment() && arena.segmentCount() == 1u,
        "unpublished empty growth rolls back to the seed topology");
    const auto allocation = arena.allocate(128u, 16u);
    ok &= expect(allocation.valid() && !arena.removeLastEmptySegment(),
        "published or seed storage cannot be removed by rollback");

    ChainedArenaAllocator unbounded{1u};
    for (std::uint16_t segment = 1u; segment <= 17u; ++segment)
    {
        ok &= expect(unbounded.addSegment(1u) == segment,
            "default allocator is not capped by the retired 16-segment implementation limit");
    }
    ok &= expect(unbounded.segmentCount() == 18u,
        "segment topology is bounded by protocol/admission instead of an implementation magic number");

    sections.shutdown();
    return ok ? 0 : 1;
}
