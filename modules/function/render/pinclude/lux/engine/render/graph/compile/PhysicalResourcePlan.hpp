#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    [[nodiscard]] PhysicalResourcePlan planPhysicalResources(
        const RenderGraph& graph,
        std::span<const CompiledResource> resources,
        const RecordingProgram& recording,
        std::span<const SubmissionBatch> submissions,
        std::array<const QueueProperties*, 3> queues,
        ETransientAllocation allocation
    ) noexcept;

    enum class EMemoryPlacement : std::uint8_t
    {
        EXCLUDED,
        DEDICATED,
        RENDERING_SCOPE,
        DISTINCT_REQUIREMENTS,
        UNORDERED,
        ALIASED
    };

    struct MemoryPlacement final
    {
        std::uint32_t allocation;
        EMemoryPlacement reason;
    };

    // Resize transfers the connected closure of old/new allocation groups, including fixed-size members.
    void expandMemoryMigration(
        std::span<const MemoryPlacement> previous,
        std::span<const MemoryPlacement> next,
        std::span<std::uint8_t> changed
    ) noexcept;

    // Resolve actual requirements at Attach/Resize; allocation initially names a physical slot.
    [[nodiscard]] std::vector<MemoryPlacement> instantiatePlacements(
        const PhysicalResourcePlan& physical,
        std::span<const CompiledResource> resources,
        const RecordingProgram& recording,
        std::span<const nri::MemoryDesc> requirements
    ) noexcept;
} // namespace lux::render::detail
