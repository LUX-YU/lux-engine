#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>
#include <set>

namespace lux::render::detail
{
    std::uint32_t GraphCompileDiagnostics::passSlot(GraphPassId source) const noexcept
    {
        const auto found = std::ranges::find(source_passes, source);
        if (found == source_passes.end()) return kGraphInvalidIndex;
        return static_cast<std::uint32_t>(found - source_passes.begin());
    }

    namespace
    {
        std::string transitionBoundary(ETransitionBoundary boundary)
        {
            return quoteDumpText(
                boundary == ETransitionBoundary::EXPORT             ? "export"
                : boundary == ETransitionBoundary::RESTORE_IMPORTED ? "restore imported state"
                : boundary == ETransitionBoundary::RESTORE_TRANSIENT ? "restore transient state"
                : boundary == ETransitionBoundary::PUBLISH_READS ? "after writer"
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
        const auto& plan = graph.graph;
        const auto& report = graph.diagnostics;
        const auto& program = plan.recording_;
        const bool missing_passes = !plan.binding_plan_ || report.source_passes.size() != program.passes.size() ||
            report.descriptor_fields.size() != plan.binding_plan_->passes.size();
        const bool missing_resources = report.resource_keys.size() != plan.resources_.size() ||
            report.resource_reuse.size() != plan.resources_.size();
        if (missing_passes || missing_resources) return false;
        std::string output;
        const auto sourcePass = [&](const auto& transition)
        {
            return transition.previous_slot == kGraphInvalidIndex
                ? kGraphInvalidIndex : report.source_passes[transition.previous_slot].index;
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
            output = "digraph compiled::RenderGraph {\nlabel=\"Solid: recording order; dashed: compiled barriers\";\n";
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
                for (const auto& transition : program.imageBarriers(program.passes[i]))
                {
                    barrierEdge(
                        transition,
                        "mip " + std::to_string(transition.native.mipOffset) + " layer " +
                            std::to_string(transition.native.layerOffset)
                    );
                }
                for (const auto& transition : program.bufferBarriers(program.passes[i]))
                {
                    barrierEdge(transition, "whole-buffer barrier");
                }
            }
            output += "}\n";
            return sink.write(sink.destination, output);
        }
        std::set<nri::Pipeline*> pipelines;
        std::set<nri::PipelineLayout*> layouts;
        std::size_t image_barriers = program.after.images.size() + program.transient_exits.size();
        std::size_t buffer_barriers = program.after.buffers.size() + program.buffer_read_releases.size();
        std::size_t dynamic_raster_passes = 0;
        for (const auto& pass : program.passes)
        {
            image_barriers += pass.entry_images.count;
            buffer_barriers += pass.entry_buffers.count;
            if (const auto* raster = plan.rasterCommands(pass))
            {
                dynamic_raster_passes += std::holds_alternative<RasterListCommands>(raster->draw);
            }
            const auto& native = plan.programs()[pass.binding_slot];
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
            const auto kind = std::visit([](auto slot) { return slot.kind; }, pass.commands);
            output +=
                "{\"source\":" + std::to_string(report.source_passes[p].index) +
                ",\"unit\":" + std::to_string(pass.unit) +
                ",\"kind\":" + std::to_string(static_cast<unsigned>(kind)) +
                ",\"group\":" + std::to_string(pass.rendering_group) +
                ",\"condition_region\":" + std::to_string(pass.condition_region) +
                ",\"queue_role\":" + std::to_string(static_cast<unsigned>(pass.queue)) +
                ",\"rendering_boundary\":" + quoteDumpText(boundaries[static_cast<unsigned>(pass.rendering_boundary)]) +
                ",\"descriptors\":[";
            const auto& layout = plan.binding_plan_->pass(pass.binding_slot);
            const auto& fields = report.descriptor_fields[pass.binding_slot];
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
            const auto images = program.imageBarriers(pass);
            for (std::size_t i = 0; i < images.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += imageTransitionDump(images[i], sourcePass(images[i]));
            }
            output += "],\"buffer_transitions\":[";
            const auto buffers = program.bufferBarriers(pass);
            for (std::size_t i = 0; i < buffers.size(); ++i)
            {
                if (i)
                {
                    output += ',';
                }
                output += bufferTransitionDump(buffers[i], sourcePass(buffers[i]));
            }
            output += "]}";
        }
        output += "],\"physical_resources\":[";
        constexpr std::string_view reuse_reasons[]{
            "unused",
            "dynamic extent",
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
        static_assert(std::size(reuse_reasons) == static_cast<unsigned>(EResourceReuse::REUSED) + 1);
        for (std::size_t i = 0; i < plan.physical_resources_.resource_slots.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            output += "{\"resource\":" + std::to_string(i) +
                      ",\"slot\":" + std::to_string(plan.physical_resources_.resource_slots[i].index) + ",\"reason\":" +
                      quoteDumpText(reuse_reasons[static_cast<unsigned>(report.resource_reuse[i])]) + "}";
        }
        output += "],\"alias_use_frontiers\":[";
        const auto& physical = plan.physical_resources_;
        bool first_point = true;
        for (std::size_t s = 0; s < physical.alias_frontiers.size(); ++s)
        {
            const auto& queues = physical.alias_frontiers[s].queues;
            for (std::size_t q = 0; q < queues.size(); ++q)
            {
                const auto& queue = queues[q];
                for (const auto& point : std::span{physical.alias_points}.subspan(queue.first_point, queue.point_count))
                {
                    if (!first_point) output += ',';
                    first_point = false;
                    output += "{\"slot\":" + std::to_string(s) + ",\"queue\":" + std::to_string(q) +
                        ",\"range\":" + dumpRange(point.range) + ",\"first_pass\":" + std::to_string(point.first_pass) +
                        ",\"last_pass\":" + std::to_string(point.last_pass) +
                        ",\"access\":" + std::to_string(static_cast<unsigned>(point.scope.access)) +
                        ",\"stages\":" + std::to_string(static_cast<unsigned>(point.scope.stages)) +
                        ",\"writes\":" + (point.writes ? "true" : "false") +
                        ",\"needs_entry\":" + (point.needs_entry ? "true" : "false") + "}";
                }
            }
        }
        output += "],\"alias_submission_scopes\":[";
        bool first_scope = true;
        for (std::size_t s = 0; s < physical.alias_frontiers.size(); ++s)
        {
            const auto& frontier = physical.alias_frontiers[s];
            for (std::size_t q = 0; q < frontier.queues.size(); ++q)
            {
                const auto& entry = frontier.queues[q];
                if (!entry.point_count) continue;
                if (!first_scope) output += ',';
                first_scope = false;
                output += "{\"slot\":" + std::to_string(s) + ",\"queue\":" + std::to_string(q) +
                    ",\"entry_pass\":" + std::to_string(entry.barrier_pass) +
                    ",\"last_submission\":" + std::to_string(entry.last_submission) +
                    ",\"entry_stages\":" + std::to_string(static_cast<unsigned>(entry.destination.stages)) +
                    ",\"direct_waited_through\":[";
                for (std::size_t source = 0; source < entry.waited_through.size(); ++source)
                {
                    if (source) output += ',';
                    const auto batch = entry.waited_through[source];
                    output += batch == kGraphInvalidIndex ? "null" : std::to_string(batch);
                }
                output += "]}";
            }
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
                      ",\"output_begin\":" + std::to_string(batch.outputBegin()) +
                      ",\"reusable_recording\":" + (batch.recordings.front().reusable ? "true" : "false") +
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
            const auto source_input = region.source == EConditionSource::HISTORY_VALID
                ? plan.histories_[region.input_slot].description.previous.index : region.input_slot;
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
        output += "],\"transient_exit_transitions\":[";
        for (std::size_t i = 0; i < program.transient_exits.size(); ++i)
        {
            if (i) output += ',';
            const auto& transition = program.transient_exits[i];
            output += imageTransitionDump(transition, sourcePass(transition));
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
        output += "],\"buffer_read_releases\":[";
        for (std::size_t i = 0; i < program.buffer_read_releases.size(); ++i)
        {
            if (i) output += ',';
            const auto& release = program.buffer_read_releases[i];
            output += bufferTransitionDump(release, sourcePass(release));
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
                      quoteDumpText(report.resource_keys[i]) + ",\"allocation\":" +
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
