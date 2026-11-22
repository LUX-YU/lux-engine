#pragma once

#include <lux/engine/render/extensions/graph/Diagnostics.hpp>

#include <lux/engine/render/graph/compile/GraphDefinition.hpp>

namespace lux::render
{
    enum class EGraphHazard : std::uint8_t
    {
        RAW,
        WAR,
        WAW
    };

    struct GraphHazard final
    {
        GraphPassId before;
        GraphPassId after;
        GraphResourceId resource;
        VGraphRange range;
        EGraphHazard kind = EGraphHazard::RAW;
        std::uint32_t before_version = 0;
        std::uint32_t after_version = 0;
        bool operator==(const GraphHazard&) const noexcept = default;
    };

    // A resolved range's producer/consumer relation, including initial contents
    // (invalid producer) and output observation (invalid consumer). Fallback reads
    // retain their selected source; no borrowed GraphPass/GraphResource pointers.
    struct GraphVersionAccess final
    {
        ResourceVersion version;
        VGraphRange range;
        GraphPassId producer;
        GraphPassId consumer;
        bool fallback = false;
        bool operator==(const GraphVersionAccess&) const noexcept = default;
    };

    struct GraphLifetime final
    {
        ResourceVersion version;
        VGraphRange range;
        EGraphOrigin origin = EGraphOrigin::TRANSIENT;
        std::uint32_t first = 0;
        std::uint32_t last = 0; // inclusive; N denotes post-execution export observation
        bool operator==(const GraphLifetime&) const noexcept = default;
    };

    struct RenderGraphAnalysis final
    {
        std::vector<GraphPassId> execution_order;
        std::vector<GraphDependency> dependencies;
        std::vector<GraphHazard> hazards;
        std::vector<GraphVersionAccess> versions;
        std::vector<GraphLifetime> lifetimes;
        // Live CPU parameter inputs are not GPU memory reads and introduce no synthetic barriers.
        bool operator==(const RenderGraphAnalysis&) const noexcept = default;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<RenderGraphAnalysis> analyzeRenderGraph(const GraphDefinition& definition
    ) noexcept;
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpGraph(
        const GraphDefinition&,
        const RenderGraphAnalysis&,
        EGraphDumpFormat,
        DumpSink
    ) noexcept;
} // namespace lux::render
