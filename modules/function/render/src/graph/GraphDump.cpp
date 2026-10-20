#include <algorithm>
#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/GraphDump.hpp>

namespace lux::render
{
    bool dumpRenderGraph(
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        EGraphDumpFormat format,
        GraphDumpSink sink
    ) noexcept
    {
        if (!sink.write || format > EGraphDumpFormat::DOT)
        {
            return false;
        }
        std::string output;
        const auto live = [&](std::uint32_t pass)
        { return std::ranges::find(analysis.execution_order, GraphPassId{pass}) != analysis.execution_order.end(); };
        if (format == EGraphDumpFormat::DOT)
        {
            output = "digraph RenderGraph {\n";
            for (std::uint32_t i = 0; i < graph.passes().size(); ++i)
            {
                output += "p" + std::to_string(i) + " [label=" + detail::quoteDumpText(graph.passes()[i].key) +
                          (live(i) ? ",style=solid];\n" : ",style=dashed];\n");
            }
            for (const auto& edge : analysis.dependencies)
            {
                output += "p" + std::to_string(edge.before.index) + " -> p" + std::to_string(edge.after.index) + ";\n";
            }
            output += "}\n";
        }
        else
        {
            output = "{\"passes\":[";
            for (std::uint32_t i = 0; i < graph.passes().size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += "{\"id\":" + std::to_string(i) + ",\"key\":" + detail::quoteDumpText(graph.passes()[i].key) +
                          ",\"live\":" + (live(i) ? "true" : "false") + "}";
            }
            output += "],\"resources\":[";
            for (std::uint32_t i = 0; i < graph.resources().size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += "{\"id\":" + std::to_string(i) +
                          ",\"key\":" + detail::quoteDumpText(graph.resources()[i].key) +
                          ",\"origin\":" + std::to_string(static_cast<unsigned>(graph.resources()[i].origin)) + "}";
            }
            output += "],\"versions\":[";
            for (std::size_t i = 0; i < analysis.versions.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& use = analysis.versions[i];
                output += "{\"resource\":" + std::to_string(use.version.resource.index) +
                          ",\"version\":" + std::to_string(use.version.number) +
                          ",\"producer\":" + std::to_string(use.producer.index) +
                          ",\"consumer\":" + std::to_string(use.consumer.index) +
                          ",\"fallback\":" + (use.fallback ? "true" : "false") +
                          ",\"range\":" + detail::dumpRange(use.range) + "}";
            }
            output += "],\"hazards\":[";
            constexpr std::string_view names[]{"RAW", "WAR", "WAW"};
            for (std::size_t i = 0; i < analysis.hazards.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& hazard = analysis.hazards[i];
                output += "{\"kind\":" + detail::quoteDumpText(names[static_cast<unsigned>(hazard.kind)]) +
                          ",\"before\":" + std::to_string(hazard.before.index) +
                          ",\"after\":" + std::to_string(hazard.after.index) +
                          ",\"resource\":" + std::to_string(hazard.resource.index) +
                          ",\"range\":" + detail::dumpRange(hazard.range) + "}";
            }
            output += "],\"parameter_inputs\":[";
            for (std::size_t i = 0; i < analysis.parameter_inputs.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += std::to_string(analysis.parameter_inputs[i].index);
            }
            output += "]}\n";
        }
        return sink.write(sink.destination, output);
    }
} // namespace lux::render
