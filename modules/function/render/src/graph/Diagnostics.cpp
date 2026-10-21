#include <algorithm>
#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/DumpText.hpp>

namespace lux::render
{
    bool dumpGraph(
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        EGraphDumpFormat format,
        DumpSink sink
    ) noexcept
    {
        if (!sink.write || format > EGraphDumpFormat::DOT)
        {
            return false;
        }
        std::string output;
        std::vector<std::uint32_t> positions(graph.passes().size(), kGraphInvalidIndex);
        for (std::uint32_t position = 0; position < analysis.execution_order.size(); ++position)
        {
            positions[analysis.execution_order[position].index] = position;
        }
        const auto live = [&](std::uint32_t pass) { return positions[pass] != kGraphInvalidIndex; };
        if (format == EGraphDumpFormat::DOT)
        {
            output = "digraph RenderGraph {\nlabel=\"Resource and explicit dependencies; dashed nodes are culled\";\n";
            for (std::uint32_t i = 0; i < graph.passes().size(); ++i)
            {
                output += "p" + std::to_string(i) + " [label=" + detail::quoteDumpText(graph.passes()[i].key) +
                          (live(i) ? ",style=solid];\n" : ",style=dashed];\n");
            }
            constexpr std::string_view hazards[]{"RAW", "WAR", "WAW"};
            for (const auto& edge : analysis.hazards)
            {
                const auto label = std::string(hazards[static_cast<unsigned>(edge.kind)]) + " resource " +
                                   std::to_string(edge.resource.index) + " " + detail::dumpRange(edge.range);
                output += "p" + std::to_string(edge.before.index) + " -> p" + std::to_string(edge.after.index) +
                          " [label=" + detail::quoteDumpText(label) + "];\n";
            }
            for (const auto& edge : graph.dependencies())
            {
                output += "p" + std::to_string(edge.before.index) + " -> p" + std::to_string(edge.after.index) +
                          " [label=\"explicit order\",style=dotted];\n";
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
                          ",\"live\":" + (live(i) ? "true" : "false") +
                          ",\"execution_position\":" + std::to_string(positions[i]) + ",\"retention\":" +
                          detail::quoteDumpText(
                              !live(i)                        ? "not reachable from an export or side effect"
                              : graph.passes()[i].side_effect ? "side effect root"
                                                              : "required by an export or retained consumer"
                          ) +
                          ",\"accesses\":[";
                const auto& uses = graph.passes()[i].uses;
                for (std::size_t u = 0; u < uses.size(); ++u)
                {
                    if (u)
                    {
                        output += ',';
                    }
                    const auto& use = uses[u];
                    output += "{\"resource\":" + std::to_string(use.version.resource.index) +
                              ",\"version\":" + std::to_string(use.version.number) +
                              ",\"predecessor\":" + std::to_string(use.input_version) +
                              ",\"access\":" + std::to_string(static_cast<unsigned>(use.access)) +
                              ",\"usage\":" + std::to_string(static_cast<unsigned>(use.usage)) +
                              ",\"range\":" + detail::dumpRange(use.range) + "}";
                }
                output += "]}";
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
                          ",\"origin\":" + std::to_string(static_cast<unsigned>(graph.resources()[i].origin)) +
                          ",\"initial_use\":" + std::to_string(static_cast<unsigned>(graph.resources()[i].initial_use));
                const auto& description = graph.resources()[i].description;
                if (const auto* buffer = std::get_if<GraphBufferDesc>(&description))
                {
                    output += ",\"bytes\":" + std::to_string(buffer->size);
                }
                else
                {
                    const auto& image = std::get<GraphImageDesc>(description);
                    output +=
                        ",\"width\":" + std::to_string(image.width) + ",\"height\":" + std::to_string(image.height) +
                        ",\"mips\":" + std::to_string(image.mips) + ",\"layers\":" + std::to_string(image.layers) +
                        ",\"samples\":" + std::to_string(image.samples) +
                        ",\"format\":" + std::to_string(static_cast<unsigned>(image.format)) +
                        ",\"target_relative\":" + (image.extent.target_relative ? "true" : "false");
                }
                output += ",\"initial_ranges\":[";
                for (std::size_t range = 0; range < graph.resources()[i].initial_ranges.size(); ++range)
                {
                    if (range)
                    {
                        output += ',';
                    }
                    output += detail::dumpRange(graph.resources()[i].initial_ranges[range]);
                }
                output += "]}";
            }
            output += "],\"histories\":[";
            for (std::size_t i = 0; i < graph.histories().size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& history = graph.histories()[i];
                output +=
                    "{\"previous\":" + std::to_string(history.previous.index) +
                    ",\"current\":" + std::to_string(history.current.index) +
                    ",\"distance\":" + std::to_string(history.distance) +
                    ",\"canonical_use\":" + std::to_string(static_cast<unsigned>(history.canonical_use)) +
                    ",\"publication\":" + (history.publication ? std::to_string(history.publication->number) : "null") +
                    "}";
            }
            output += "],\"condition_inputs\":[";
            for (std::size_t i = 0; i < graph.conditions().size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += detail::quoteDumpText(graph.conditions()[i]);
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
            output += "],\"exports\":[";
            for (std::size_t i = 0; i < graph.outputs().size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& value = graph.outputs()[i];
                output += "{\"resource\":" + std::to_string(value.version.resource.index) +
                          ",\"version\":" + std::to_string(value.version.number) +
                          ",\"range\":" + detail::dumpRange(value.range) + ",\"final_use\":" +
                          (value.final_use ? std::to_string(static_cast<unsigned>(*value.final_use)) : "null") + "}";
            }
            output += "],\"lifetimes\":[";
            for (std::size_t i = 0; i < analysis.lifetimes.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& value = analysis.lifetimes[i];
                output += "{\"resource\":" + std::to_string(value.version.resource.index) +
                          ",\"version\":" + std::to_string(value.version.number) +
                          ",\"range\":" + detail::dumpRange(value.range) + ",\"first\":" + std::to_string(value.first) +
                          ",\"last\":" + std::to_string(value.last) + "}";
            }
            output += "],\"explicit_dependencies\":[";
            for (std::size_t i = 0; i < graph.dependencies().size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& edge = graph.dependencies()[i];
                output += "{\"before\":" + std::to_string(edge.before.index) +
                          ",\"after\":" + std::to_string(edge.after.index) + "}";
            }
            output += "]}\n";
        }
        return sink.write(sink.destination, output);
    }
} // namespace lux::render
