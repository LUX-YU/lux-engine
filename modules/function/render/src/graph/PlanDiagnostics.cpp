#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>

namespace lux::render::detail
{
    namespace
    {
        std::string transitionBoundary(ETransitionBoundary boundary)
        {
            return quoteDumpText(
                boundary == ETransitionBoundary::EXPORT             ? "export"
                : boundary == ETransitionBoundary::RESTORE_IMPORTED ? "restore imported state"
                                                                    : "before pass"
            );
        }

        std::string attachmentDump(const RasterAttachment& value)
        {
            std::string output = "{\"resource\":" + std::to_string(value.access.version.resource.index) +
                                 ",\"version\":" + std::to_string(value.access.version.number) +
                                 ",\"range\":" + dumpRange(value.access.range) +
                                 ",\"load\":" + std::to_string(static_cast<unsigned>(value.load)) +
                                 ",\"store\":" + std::to_string(static_cast<unsigned>(value.store)) + ",\"resolve\":";
            if (value.resolve)
            {
                output += "{\"resource\":" + std::to_string(value.resolve->version.resource.index) +
                          ",\"range\":" + dumpRange(value.resolve->range) + "}";
            }
            else
            {
                output += "null";
            }
            return output + "}";
        }

        std::string imageTransitionDump(const ImageTransition& transition)
        {
            const auto& barrier = transition.native;
            return "{\"resource\":" + std::to_string(transition.resource) +
                   ",\"previous_pass\":" + std::to_string(transition.previous.index) +
                   ",\"boundary\":" + transitionBoundary(transition.boundary) + ",\"source\":" +
                   quoteDumpText(transition.previous.index == kGraphInvalidIndex ? "initial state" : "pass access") +
                   ",\"planes\":" + std::to_string(static_cast<unsigned>(barrier.planes)) +
                   ",\"mip\":" + std::to_string(barrier.mipOffset) + ",\"mips\":" + std::to_string(barrier.mipNum) +
                   ",\"layer\":" + std::to_string(barrier.layerOffset) +
                   ",\"layers\":" + std::to_string(barrier.layerNum) +
                   ",\"source_stages\":" + std::to_string(static_cast<unsigned>(barrier.before.stages)) +
                   ",\"destination_stages\":" + std::to_string(static_cast<unsigned>(barrier.after.stages)) +
                   ",\"source_access\":" + std::to_string(static_cast<unsigned>(barrier.before.access)) +
                   ",\"destination_access\":" + std::to_string(static_cast<unsigned>(barrier.after.access)) +
                   ",\"before\":" + std::to_string(static_cast<unsigned>(barrier.before.layout)) +
                   ",\"after\":" + std::to_string(static_cast<unsigned>(barrier.after.layout)) + "}";
        }

        std::string bufferTransitionDump(const BufferTransition& transition)
        {
            return "{\"resource\":" + std::to_string(transition.resource) +
                   ",\"previous_pass\":" + std::to_string(transition.previous.index) +
                   ",\"boundary\":" + transitionBoundary(transition.boundary) +
                   ",\"scope\":\"whole physical buffer\",\"source_stages\":" +
                   std::to_string(static_cast<unsigned>(transition.native.before.stages)) +
                   ",\"destination_stages\":" + std::to_string(static_cast<unsigned>(transition.native.after.stages)) +
                   ",\"before\":" + std::to_string(static_cast<unsigned>(transition.native.before.access)) +
                   ",\"after\":" + std::to_string(static_cast<unsigned>(transition.native.after.access)) + "}";
        }
    } // namespace

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
            output = "digraph RenderPlan {\nlabel=\"Solid: recording order; dashed: compiled barriers\";\n";
            for (std::size_t i = 0; i < program.passes.size(); ++i)
            {
                output += "p" + std::to_string(i) + " [label=\"pass " + std::to_string(i) + " / group " +
                          std::to_string(program.passes[i].rendering_group) + "\"];\n";
                if (i)
                {
                    output += "p" + std::to_string(i - 1) + " -> p" + std::to_string(i) + " [label=\"record next\"];\n";
                }
                const auto barrierEdge = [&](const auto& transition, std::string detail)
                {
                    if (transition.previous.index == kGraphInvalidIndex)
                    {
                        return;
                    }
                    const auto previous = plan.pass_indices_[transition.previous.index];
                    output += "p" + std::to_string(previous) + " -> p" + std::to_string(i) + " [style=dashed,label=" +
                              quoteDumpText("resource " + std::to_string(transition.resource) + " " + detail) + "];\n";
                };
                for (const auto& transition : program.passes[i].before.images)
                {
                    barrierEdge(
                        transition,
                        "mip " + std::to_string(transition.native.mipOffset) + " layer " +
                            std::to_string(transition.native.layerOffset)
                    );
                }
                for (const auto& transition : program.passes[i].before.buffers)
                {
                    barrierEdge(transition, "whole-buffer barrier");
                }
            }
            output += "}\n";
            return sink.write(sink.destination, output);
        }
        output = "{\"queue\":\"graphics\",\"submission_policy\":\"merge prepared instances in FrameExecution; actual "
                 "batches in dumpSubmissions\",\"alias\":false,\"history_count\":" +
                 std::to_string(plan.histories_.size()) +
                 ","
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
            output +=
                "{\"source\":" + std::to_string(pass.source_pass) + ",\"kind\":" + std::to_string(pass.work.index()) +
                ",\"group\":" + std::to_string(pass.rendering_group) +
                ",\"conditional\":" + (pass.condition ? "true" : "false") +
                ",\"rendering_boundary\":" + quoteDumpText(boundaries[static_cast<unsigned>(pass.rendering_boundary)]) +
                ",\"descriptors\":[";
            const auto& layout = plan.descriptor_plan_->passes[pass.source_pass];
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
                output += imageTransitionDump(pass.before.images[i]);
            }
            output += "],\"buffer_transitions\":[";
            for (std::size_t i = 0; i < pass.before.buffers.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += bufferTransitionDump(pass.before.buffers[i]);
            }
            output += "]}";
        }
        output += "],\"final_image_transitions\":[";
        for (std::size_t i = 0; i < program.after.images.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            output += imageTransitionDump(program.after.images[i]);
        }
        output += "],\"final_buffer_transitions\":[";
        for (std::size_t i = 0; i < program.after.buffers.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            output += bufferTransitionDump(program.after.buffers[i]);
        }
        output += "],\"groups\":[";
        for (std::size_t i = 0; i < program.groups.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& group = program.groups[i];
            output += "{\"first\":" + std::to_string(group.first_pass) +
                      ",\"passes\":" + std::to_string(group.pass_count) +
                      ",\"color_count\":" + std::to_string(group.colors.size()) + ",\"colors\":[";
            for (std::size_t color = 0; color < group.colors.size(); ++color)
            {
                if (color)
                {
                    output += ',';
                }
                output += attachmentDump(group.colors[color]);
            }
            output += "],\"depth\":" + (group.depth ? attachmentDump(*group.depth) : "null") + "}";
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
