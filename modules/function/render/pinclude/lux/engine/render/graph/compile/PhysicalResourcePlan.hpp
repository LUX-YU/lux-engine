#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{

    enum class EMemoryPlacement : std::uint8_t
    {
        EXCLUDED,
        DEDICATED,
        RENDERING_SCOPE,
        DISTINCT_REQUIREMENTS,
        INCOMPATIBLE_LAYOUT,
        UNORDERED,
        ALIASED
    };

    struct AllocationRootSlot final
    {
        std::uint32_t index;
    };

    struct MemoryGroupId final
    {
        std::uint32_t index;
    };

    struct RootMemoryPlacement final
    {
        AllocationRootSlot root;
        EMemoryPlacement reason;
    };

    struct BoundMemoryPlacement final
    {
        MemoryGroupId group;
        EMemoryPlacement reason;
    };

    // Select only handoffs in the actual allocation groups. Cross-queue ordering is established by the submit DAG.
    [[nodiscard]] std::vector<std::optional<nri::GlobalBarrierDesc>> instantiateAliasBarriers(
        const PhysicalResourcePlan& physical, std::span<const BoundMemoryPlacement> placements, std::size_t pass_count
    ) noexcept;

    // Resize transfers the connected closure of old/new allocation groups, including fixed-size members.
    void expandMemoryMigration(
        std::span<const BoundMemoryPlacement> previous,
        std::span<const RootMemoryPlacement> next,
        std::span<std::uint8_t> changed
    ) noexcept;

    // Resolve actual requirements at Attach/Resize; each group is identified by its root physical slot.
    [[nodiscard]] std::vector<RootMemoryPlacement> instantiatePlacements(
        const PhysicalResourcePlan& physical,
        std::span<const nri::MemoryDesc> requirements,
        nri::GraphicsAPI backend
    ) noexcept;
} // namespace lux::render::detail
