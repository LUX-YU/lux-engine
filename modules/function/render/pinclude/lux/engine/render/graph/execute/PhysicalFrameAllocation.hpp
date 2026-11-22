#pragma once

#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/execute/FrameResourceBinding.hpp>

namespace lux::render::detail
{
    struct LivePhysicalPlacement final
    {
        std::span<const FrameResourceBinding> bindings;
        std::span<const BoundMemoryPlacement> placements;
    };

    struct PhysicalFrameContract final
    {
        const compiled::RenderGraph& graph;
        const TargetDesc& target;
        std::optional<LivePhysicalPlacement> live;
    };

    struct GraphBindingChange final
    {
        std::uint32_t index;
        FrameResourceBinding binding;
    };

    enum class EAllocationOwner { LIVE, CANDIDATE };

    struct PhysicalMemorySource final
    {
        EAllocationOwner owner;
        std::uint32_t index;
    };

    struct PhysicalFrameChanges final
    {
        std::vector<GraphBindingChange> resources;
        std::vector<std::uint8_t> replaced_slots;
        std::vector<BoundMemoryPlacement> placements;
        std::vector<PhysicalMemorySource> memory_sources;
    };

    [[nodiscard]] RenderResult<nri::BufferDesc>
    graphBufferDescription(DeviceSession&, const CompiledResource&, std::uint64_t bytes) noexcept;

    [[nodiscard]] RenderResult<nri::TextureDesc>
    graphTextureDescription(DeviceSession&, const CompiledResource&, const TargetDesc&) noexcept;

    [[nodiscard]] RenderResult<FrameResourceBinding> createGraphResource(
        DeviceSession&, const CompiledResource&, const TargetDesc&, ETransientAllocation
    ) noexcept;
} // namespace lux::render::detail
