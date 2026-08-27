#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>
#include <lux/engine/render/resources/mesh/SparseInstanceStream.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{
    bool expect(bool condition, const char* message)
    {
        if (condition)
            return true;
        std::fprintf(stderr, "instance sparse contract: %s\n", message);
        return false;
    }
}

int
main()
{
    using namespace lux::render;

    bool ok = true;
    ok &= expect(instancePageIndex(16'383u) == 0u && instancePageOffset(16'383u) == 16'383u, "last slot of page zero");
    ok &= expect(instancePageIndex(16'384u) == 1u && instancePageOffset(16'384u) == 0u, "first slot of page one");
    ok &= expect(
        instanceRootIndex(1'048'575u) == 0u && instanceLeafIndex(1'048'575u) == 63u &&
            instancePageOffset(1'048'575u) == 16'383u,
        "last slot below the retired 20-bit boundary"
    );
    ok &= expect(
        instanceRootIndex(1'048'576u) == 0u && instanceLeafIndex(1'048'576u) == 64u &&
            instancePageOffset(1'048'576u) == 0u,
        "first slot above the retired 20-bit boundary"
    );
    ok &= expect(
        instanceRootIndex(std::numeric_limits<std::uint32_t>::max() - 1u) == 511u &&
            instanceLeafIndex(std::numeric_limits<std::uint32_t>::max() - 1u) == 511u &&
            instancePageOffset(std::numeric_limits<std::uint32_t>::max() - 1u) == 16'382u,
        "last valid RenderObjectHandle slot decodes inside the two-level table");
    ok &= expect(
        InstanceSlotRegistry::nextGeneration(1u) == 2u &&
            InstanceSlotRegistry::nextGeneration(std::numeric_limits<std::uint32_t>::max()) == 0u,
        "generation wrap retires instead of aliasing an old handle");

    InstanceSlotRegistry registry;
    registry.init(2u);
    const auto first = registry.allocateObject();
    const auto second = registry.allocateObject();
    ok &= expect(
        first.index == 0u && first.gen == 1u && second.index == 1u && second.gen == 1u,
        "new physical slots and generations are deterministic");
    registry.freeObject(first);
    const auto reused = registry.allocateObject();
    ok &= expect(
        reused.index == first.index && reused.gen == first.gen + 1u,
        "free-list reuse advances generation without moving the slot");
    ok &= expect(
        !registry.isAlive(first) && registry.isAlive(reused),
        "stale generation is rejected after immediate reuse");

    return ok ? 0 : 1;
}
