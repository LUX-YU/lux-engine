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

    // Resolve actual requirements at Attach/Resize; allocation initially names a physical slot.
    [[nodiscard]] std::vector<MemoryPlacement> instantiatePlacements(
        const PhysicalResourcePlan& physical,
        std::span<const CompiledResource> resources,
        const RecordingProgram& recording,
        std::span<const nri::MemoryDesc> requirements
    ) noexcept;
} // namespace lux::render::detail
