#pragma once

#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    struct GraphDescriptorDiagnostics final
    {
        std::string name;
        std::uint32_t set, binding, count, stages, record_bytes;
        std::uint8_t kind;
        bool runtime_records;
    };

    struct GraphCompileDiagnostics final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC std::uint32_t passSlot(GraphPassId) const noexcept;
        std::vector<GraphPassId> source_passes;
        std::vector<std::string> resource_keys;
        std::vector<EResourceReuse> resource_reuse;
        std::vector<std::vector<GraphDescriptorDiagnostics>> descriptor_fields;
    };

    struct LUX_RENDER_PUBLIC PreparedRenderGraph final
    {
        compiled::RenderGraph graph;
        GraphCompileDiagnostics diagnostics;
    };
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;
} // namespace lux::render::detail
