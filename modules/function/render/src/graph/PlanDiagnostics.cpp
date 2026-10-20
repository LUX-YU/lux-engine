#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>

namespace lux::render::detail
{
    bool dumpPlan(const RenderPlan& plan, EGraphDumpFormat format, DumpSink sink) noexcept
    {
        if (!sink.write || format > EGraphDumpFormat::DOT)
        {
            return false;
        }
        std::string output;
        const auto& program = plan.recording_;
        constexpr std::string_view boundaries[]{
            "non-raster",
            "first or follows non-raster",
            "attachment/range/load-store/resolve differs",
            "resource barrier required",
            "merged with preceding raster"
        };
        if (format == EGraphDumpFormat::DOT)
        {
            output = "digraph RenderPlan {\n";
            for (std::size_t i = 0; i < program.passes.size(); ++i)
            {
                output += "p" + std::to_string(i) + " [label=\"pass " + std::to_string(i) + " / group " +
                          std::to_string(program.passes[i].rendering_group) + "\"];\n";
                if (i)
                {
                    output += "p" + std::to_string(i - 1) + " -> p" + std::to_string(i) + ";\n";
                }
            }
            output += "}\n";
            return sink.write(sink.destination, output);
        }
        output = "{\"queue\":\"graphics\",\"submit_owner\":\"FrameExecution\",\"alias\":false,\"history\":false,"
                 "\"async_compute\":false,\"counts\":\"compiled, not measured\",\"pipelines\":" +
                 std::to_string(plan.pipelines_.size()) + ",\"layouts\":" + std::to_string(plan.layouts_.size()) +
                 ",\"passes\":[";
        for (std::size_t p = 0; p < program.passes.size(); ++p)
        {
            if (p)
            {
                output += ',';
            }
            const auto& pass = program.passes[p];
            output += "{\"source\":" + std::to_string(pass.layout_program) +
                      ",\"kind\":" + std::to_string(pass.work.index()) +
                      ",\"group\":" + std::to_string(pass.rendering_group) + ",\"rendering_boundary\":" +
                      quoteDumpText(boundaries[static_cast<unsigned>(pass.rendering_boundary)]) + ",\"descriptors\":[";
            const auto& layout = plan.descriptor_plan_->programs[pass.layout_program];
            for (std::size_t i = 0; i < layout.bindings.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& binding = layout.bindings[i];
                output += "{\"name\":" + quoteDumpText(binding.name) + ",\"set\":" + std::to_string(binding.set) +
                          ",\"binding\":" + std::to_string(binding.binding) +
                          ",\"count\":" + std::to_string(binding.count) +
                          ",\"kind\":" + std::to_string(static_cast<unsigned>(binding.kind)) +
                          ",\"stages\":" + std::to_string(binding.stages) +
                          ",\"record_bytes\":" + std::to_string(binding.block.size) +
                          ",\"runtime_records\":" + (binding.block.runtime_records ? "true" : "false") + "}";
            }
            output += "],\"descriptor_inputs\":[";
            for (std::size_t i = 0; i < layout.uses.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& use = layout.uses[i];
                output += "{\"resource\":" + std::to_string(use.resource) + ",\"field\":" + std::to_string(use.field) +
                          ",\"element\":" + std::to_string(use.element) +
                          ",\"sampler_input\":" + std::to_string(use.sampler) + ",\"range\":" + dumpRange(use.range) +
                          "}";
            }
            output += "],\"image_transitions\":[";
            for (std::size_t i = 0; i < pass.before.images.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& transition = pass.before.images[i];
                const auto& barrier = transition.native;
                output += "{\"resource\":" + std::to_string(transition.resource) +
                          ",\"previous_pass\":" + std::to_string(transition.previous.index) +
                          ",\"planes\":" + std::to_string(static_cast<unsigned>(barrier.planes)) +
                          ",\"source_stages\":" + std::to_string(static_cast<unsigned>(barrier.before.stages)) +
                          ",\"destination_stages\":" + std::to_string(static_cast<unsigned>(barrier.after.stages)) +
                          ",\"mip\":" + std::to_string(barrier.mipOffset) +
                          ",\"layer\":" + std::to_string(barrier.layerOffset) +
                          ",\"before\":" + std::to_string(static_cast<unsigned>(barrier.before.layout)) +
                          ",\"after\":" + std::to_string(static_cast<unsigned>(barrier.after.layout)) + "}";
            }
            output += "],\"buffer_transitions\":[";
            for (std::size_t i = 0; i < pass.before.buffers.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& transition = pass.before.buffers[i];
                output +=
                    "{\"resource\":" + std::to_string(transition.resource) +
                    ",\"previous_pass\":" + std::to_string(transition.previous.index) +
                    ",\"source_stages\":" + std::to_string(static_cast<unsigned>(transition.native.before.stages)) +
                    ",\"destination_stages\":" + std::to_string(static_cast<unsigned>(transition.native.after.stages)) +
                    ",\"before\":" + std::to_string(static_cast<unsigned>(transition.native.before.access)) +
                    ",\"after\":" + std::to_string(static_cast<unsigned>(transition.native.after.access)) + "}";
            }
            output += "]}";
        }
        output += "],\"final_image_transitions\":" + std::to_string(program.after.images.size()) +
                  ",\"final_buffer_transitions\":" + std::to_string(program.after.buffers.size()) + ",\"groups\":[";
        for (std::size_t i = 0; i < program.groups.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& group = program.groups[i];
            output += "{\"first\":" + std::to_string(group.first_pass) +
                      ",\"passes\":" + std::to_string(group.pass_count) +
                      ",\"colors\":" + std::to_string(group.colors.size()) +
                      ",\"depth\":" + (group.depth ? "true" : "false") + "}";
        }
        output += "],\"resources\":[";
        for (std::size_t i = 0; i < plan.resources_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& resource = plan.resources_[i];
            output += "{\"id\":" + std::to_string(i) + ",\"key\":" + quoteDumpText(resource.key) + ",\"allocation\":" +
                      quoteDumpText(
                          !resource.used                                                   ? "culled"
                          : !resource.memory_used                                          ? "CPU-parameter-input"
                          : resource.origin == EGraphOrigin::IMPORTED || i == plan.output_ ? "borrowed"
                                                                                           : "independent-per-FIF-slot"
                      ) +
                      ",\"physical_bytes\":\"backend allocation size available after instantiation\"}";
        }
        output += "]}\n";
        return sink.write(sink.destination, output);
    }
} // namespace lux::render::detail
