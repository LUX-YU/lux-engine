#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>
#include <set>

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

        std::string imageTransitionDump(const ImageTransition& transition, std::uint32_t source_pass)
        {
            const auto& barrier = transition.native;
            return "{\"resource\":" + std::to_string(transition.resource) +
                   ",\"previous_pass\":" + std::to_string(source_pass) +
                   ",\"boundary\":" + transitionBoundary(transition.boundary) + ",\"source\":" +
                   quoteDumpText(transition.previous_slot == kGraphInvalidIndex ? "initial state" : "pass access") +
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

        std::string bufferTransitionDump(const BufferTransition& transition, std::uint32_t source_pass)
        {
            return "{\"resource\":" + std::to_string(transition.resource) +
                   ",\"previous_pass\":" + std::to_string(source_pass) +
                   ",\"boundary\":" + transitionBoundary(transition.boundary) +
                   ",\"scope\":\"whole physical buffer\",\"source_stages\":" +
                   std::to_string(static_cast<unsigned>(transition.native.before.stages)) +
                   ",\"destination_stages\":" + std::to_string(static_cast<unsigned>(transition.native.after.stages)) +
                   ",\"before\":" + std::to_string(static_cast<unsigned>(transition.native.before.access)) +
                   ",\"after\":" + std::to_string(static_cast<unsigned>(transition.native.after.access)) + "}";
        }
    } // namespace

    bool dumpPlan(const PreparedRenderGraph& graph, EGraphDumpFormat format, DumpSink sink) noexcept
    {
        if (!sink.write || format > EGraphDumpFormat::DOT)
        {
            return false;
        }
        std::string output;
        const auto& plan = graph.executable;
        const auto& program = plan.recording_;
        const auto sourcePass = [&](const auto& transition)
        {
            return transition.previous_slot == kGraphInvalidIndex
                ? kGraphInvalidIndex : graph.diagnostics.source_passes[transition.previous_slot].index;
        };
        constexpr std::string_view boundaries[]{
            "non-raster",
            "first or follows non-raster",
            "attachment/range/load-store/resolve differs",
            "resource barrier required",
            "merged with preceding raster"
        };
        if (format == EGraphDumpFormat::DOT)
        {
            output = "digraph RenderGraphExecutable {\nlabel=\"Solid: recording order; dashed: compiled barriers\";\n";
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
                    if (transition.previous_slot == kGraphInvalidIndex)
                    {
                        return;
                    }
                    const auto previous = transition.previous_slot;
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
        std::set<nri::Pipeline*> pipelines;
        std::set<nri::PipelineLayout*> layouts;
        std::size_t image_barriers = program.after.images.size();
        std::size_t buffer_barriers = program.after.buffers.size();
        std::size_t dynamic_raster_passes = 0;
        for (const auto& pass : program.passes)
        {
            image_barriers += pass.before.images.size();
            buffer_barriers += pass.before.buffers.size();
            if (const auto* raster = plan.rasterCommands(pass))
            {
                dynamic_raster_passes += std::holds_alternative<RasterListCommands>(raster->draw) ||
                                         std::holds_alternative<IndexedListCommands>(raster->draw);
            }
            const auto& native = graph.programs[pass.binding_slot];
            if (native.pipeline) pipelines.insert(native.pipeline);
            if (auto* layout = native.descriptors.layout)
            {
                layouts.insert(layout);
            }
        }
        output =
            "{\"queue\":\"compiled physical roles\",\"submission_policy\":\"merge compatible ready intervals; actual "
            "batches in dumpSubmissions\",\"allocation_mode\":" +
            std::to_string(static_cast<unsigned>(plan.physical_resources_.allocation)) +
            ",\"history_count\":" + std::to_string(plan.histories_.size()) +
            ","
            "\"counts\":\"compiled, not measured\",\"pipelines\":" +
            std::to_string(pipelines.size()) + ",\"layouts\":" + std::to_string(layouts.size()) +
            ",\"dynamic_raster_passes\":" + std::to_string(dynamic_raster_passes) +
            ",\"image_barrier_descriptions\":" + std::to_string(image_barriers) +
            ",\"buffer_barrier_descriptions\":" + std::to_string(buffer_barriers) + ",\"passes\":[";
        for (std::size_t p = 0; p < program.passes.size(); ++p)
        {
            if (p)
            {
                output += ',';
            }
            const auto& pass = program.passes[p];
            output +=
                "{\"source\":" + std::to_string(graph.diagnostics.source_passes[p].index) +
                ",\"unit\":" + std::to_string(pass.unit) +
                ",\"kind\":" + std::to_string(static_cast<unsigned>(pass.kind)) +
                ",\"group\":" + std::to_string(pass.rendering_group) +
                ",\"condition_region\":" + std::to_string(pass.condition_region) +
                ",\"queue_role\":" + std::to_string(static_cast<unsigned>(pass.queue)) +
                ",\"rendering_boundary\":" + quoteDumpText(boundaries[static_cast<unsigned>(pass.rendering_boundary)]) +
                ",\"descriptors\":[";
            const auto& layout = plan.binding_plan_->pass(pass.binding_slot);
            const auto& fields = graph.diagnostics.descriptor_fields[pass.binding_slot];
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                const auto& binding = fields[i];
                output += "{\"name\":" + quoteDumpText(binding.name) + ",\"set\":" + std::to_string(binding.set) +
                          ",\"binding\":" + std::to_string(binding.binding) +
                          ",\"count\":" + std::to_string(binding.count) +
                          ",\"kind\":" + std::to_string(static_cast<unsigned>(binding.kind)) +
                          ",\"stages\":" + std::to_string(binding.stages) +
                          ",\"record_bytes\":" + std::to_string(binding.record_bytes) +
                          ",\"runtime_records\":" + (binding.runtime_records ? "true" : "false") + "}";
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
                          ",\"sampler_input\":" + std::to_string(use.sampler) +
                          ",\"history_fallback\":" + std::to_string(use.fallback) +
                          ",\"range\":" + dumpRange(use.range) + "}";
            }
            output += "],\"image_transitions\":[";
            for (std::size_t i = 0; i < pass.before.images.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += imageTransitionDump(pass.before.images[i], sourcePass(pass.before.images[i]));
            }
            output += "],\"buffer_transitions\":[";
            for (std::size_t i = 0; i < pass.before.buffers.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += bufferTransitionDump(pass.before.buffers[i], sourcePass(pass.before.buffers[i]));
            }
            output += "]}";
        }
        output += "],\"physical_resources\":[";
        constexpr std::string_view reuse_reasons[]{
            "unused",
            "external or indirect table",
            "history",
            "exported",
            "conditional",
            "initial content",
            "reuse disabled",
            "no compatible description",
            "no dependency proof",
            "object reused"
        };
        for (std::size_t i = 0; i < plan.physical_resources_.resource_slots.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            output += "{\"resource\":" + std::to_string(i) +
                      ",\"slot\":" + std::to_string(plan.physical_resources_.resource_slots[i]) + ",\"reason\":" +
                      quoteDumpText(reuse_reasons[static_cast<unsigned>(graph.diagnostics.resource_reuse[i])]) + "}";
        }
        output += "],\"physical_object_count\":" + std::to_string(plan.physical_resources_.slots.size()) +
                  ",\"submissions\":[";
        for (std::size_t i = 0; i < plan.submissions_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& batch = plan.submissions_[i];
            output += "{\"queue_role\":" + std::to_string(static_cast<unsigned>(batch.queue)) +
                      ",\"first_pass\":" + std::to_string(batch.first_pass) +
                      ",\"pass_count\":" + std::to_string(batch.pass_count) +
                      ",\"output_begin\":" + std::to_string(batch.output_begin) +
                      ",\"reusable_recording\":" + (batch.reusable_recording ? "true" : "false") +
                      ",\"dependencies\":[";
            for (std::size_t d = 0; d < batch.dependencies.size(); ++d)
            {
                if (d)
                {
                    output += ',';
                }
                output += std::to_string(batch.dependencies[d].producer);
            }
            output += "]}";
        }
        output += "],\"conditional_regions\":[";
        for (std::size_t i = 0; i < program.conditions.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& region = program.conditions[i];
            const auto source_input = region.source == EConditionSource::INPUT
                ? region.input_slot : plan.histories_[region.input_slot].description.previous.index;
            output += "{\"first_pass\":" + std::to_string(region.first_pass) +
                      ",\"pass_count\":" + std::to_string(region.pass_count) +
                      ",\"source\":" + std::to_string(static_cast<unsigned>(region.source)) +
                      ",\"input\":" + std::to_string(source_input) +
                      ",\"expected\":" + (region.expected ? "true" : "false") + "}";
        }
        output += "],\"final_image_transitions\":[";
        for (std::size_t i = 0; i < program.after.images.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            output += imageTransitionDump(program.after.images[i], sourcePass(program.after.images[i]));
        }
        output += "],\"final_buffer_transitions\":[";
        for (std::size_t i = 0; i < program.after.buffers.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            output += bufferTransitionDump(program.after.buffers[i], sourcePass(program.after.buffers[i]));
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
            const bool borrowed = resource.origin == EGraphOrigin::IMPORTED || i == plan.output_;
            output += "{\"id\":" + std::to_string(i) + ",\"key\":" +
                      quoteDumpText(graph.diagnostics.resource_keys[i]) + ",\"allocation\":" +
                      quoteDumpText(
                          !resource.used                                ? "culled"
                          : !resource.memory_used                       ? "CPU-parameter-input"
                          : borrowed                                    ? "borrowed"
                          : resource.origin == EGraphOrigin::FRAME_BUFFER ? "frame-prepared"
                                                                           : "independent-per-FIF-slot"
                      ) +
                      ",\"physical_bytes\":\"backend allocation size available after instantiation\"}";
        }
        output += "]}\n";
        return sink.write(sink.destination, output);
    }
} // namespace lux::render::detail
