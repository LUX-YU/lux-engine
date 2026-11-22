#include <lux/engine/render/device/ProgramCompileInput.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/compile/BarrierPlan.hpp>
#include <lux/engine/render/graph/compile/BuiltinPass.hpp>
#include <lux/engine/render/graph/compile/ConditionalPlan.hpp>
#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/compile/RenderingPlan.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <ranges>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<void>
        validateRasterScope(const RasterScope& scope, const RenderGraph& graph, const nri::DeviceDesc& limits) noexcept
        {
            const auto* raster = &scope;
            if (!scope.hasAttachments())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const auto& reference = raster->colors.empty() ? *raster->depth : raster->colors.front();
            const auto& reference_desc =
                std::get<GraphImageDesc>(graph.resources()[reference.access.version.resource.index].description);
            const auto& reference_range = std::get<GraphImageRange>(reference.access.range);
            auto validateAttachment = [&](const RasterAttachment& attachment, bool depth) noexcept
            {
                const auto& description =
                    std::get<GraphImageDesc>(graph.resources()[attachment.access.version.resource.index].description);
                const auto& range = std::get<GraphImageRange>(attachment.access.range);
                const bool mismatch =
                    description.samples != reference_desc.samples || range.mip_count != 1 ||
                    range.layer_count != reference_range.layer_count ||
                    (description.width >> range.first_mip) != (reference_desc.width >> reference_range.first_mip) ||
                    (description.height >> range.first_mip) != (reference_desc.height >> reference_range.first_mip) ||
                    description.extent != reference_desc.extent;
                const bool is_invalid_mode =
                    attachment.load > EAttachmentLoad::LOAD || attachment.store > EAttachmentStore::STORE;
                const bool has_unsupported_resolve = depth && attachment.resolve.has_value();
                const bool is_invalid_attachment = mismatch || is_invalid_mode || has_unsupported_resolve;
                if (is_invalid_attachment)
                {
                    return false;
                }
                if (attachment.resolve)
                {
                    const auto& resolved = std::get<GraphImageDesc>(
                        graph.resources()[attachment.resolve->version.resource.index].description
                    );
                    const auto& resolved_range = std::get<GraphImageRange>(attachment.resolve->range);
                    const bool invalid_resolve =
                        description.samples == 1 || resolved.samples != 1 || description.format != resolved.format ||
                        range.mip_count != resolved_range.mip_count ||
                        range.layer_count != resolved_range.layer_count ||
                        (description.width >> range.first_mip) != (resolved.width >> resolved_range.first_mip) ||
                        (description.height >> range.first_mip) != (resolved.height >> resolved_range.first_mip) ||
                        description.extent != resolved.extent;
                    if (invalid_resolve)
                    {
                        return false;
                    }
                }
                return true;
            };
            const bool has_invalid_color = !std::ranges::all_of(
                raster->colors, [&](const auto& color) { return validateAttachment(color, false); }
            );
            const bool has_invalid_depth = raster->depth && !validateAttachment(*raster->depth, true);
            const bool is_invalid_scope = has_invalid_color || has_invalid_depth;
            if (is_invalid_scope)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if (scope.viewport && !scope.viewport->validate())
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            return {};
        }

        nri::TextureViewDesc historyImageView(nri::Format format, const GraphImageRange& range, nri::TextureView type) noexcept
        {
            using Dim = nri::Dim_t;
            nri::TextureViewDesc view{
                nullptr, type, format, Dim(range.first_mip), Dim(range.mip_count),
                Dim(range.first_layer), Dim(range.layer_count)
            };
            view.planes = range.aspects == 1 ? nri::PlaneBits::COLOR :
                range.aspects == 2 ? nri::PlaneBits::DEPTH : nri::PlaneBits::STENCIL;
            return view;
        }

        void linkHistoryViews(
            std::span<HistoryPlan> histories, const compiled::RenderGraph& graph, const GraphBindingPlan& bindings
        ) noexcept
        {
            if (histories.empty()) return;
            const auto resources = graph.resources();
            for (const auto& pass : graph.passes())
            {
                const auto layout = bindings.pass(pass.binding_slot);
                for (std::uint32_t i = 0; i < layout.uses.size(); ++i)
                {
                    const auto& use = layout.uses[i];
                    if (use.resource == kGraphInvalidIndex) continue;
                    const auto& resource = resources[use.resource];
                    if (resource.history == kGraphInvalidIndex) continue;
                    auto& history = histories[resource.history];
                    const auto& field = layout.bindings[use.field];
                    HistoryPlan::VViewDescription view;
                    if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
                    {
                        const bool storage = use.kind == EDescriptorKind::STORAGE_IMAGE;
                        const auto type = field.image_arrayed
                            ? (storage ? nri::TextureView::STORAGE_TEXTURE_ARRAY : nri::TextureView::TEXTURE_ARRAY)
                            : (storage ? nri::TextureView::STORAGE_TEXTURE : nri::TextureView::TEXTURE);
                        view = historyImageView(imageFormat(image->format).native, std::get<GraphImageRange>(use.range), type);
                    }
                    else
                    {
                        const auto range = std::get<GraphBufferRange>(use.range);
                        view = field.bufferView(nullptr, range.offset, range.size);
                    }
                    history.views.push_back({
                        pass.binding_slot, i, EImageViewTarget::DESCRIPTOR,
                        use.resource == history.description.previous.index, view, use.binding, use.element,
                        use.fallback != kGraphInvalidIndex
                    });
                }
            }
            for (auto& history : histories)
            {
                for (auto id : {history.description.previous, history.description.current})
                {
                    const auto& resource = resources[id.index];
                    const auto* image = std::get_if<GraphImageDesc>(&resource.description);
                    if (!image) continue;
                    for (const auto& target : resource.image_views)
                    {
                        const auto type = target.range.aspects == 1 ?
                            nri::TextureView::COLOR_ATTACHMENT : nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
                        history.views.push_back({
                            target.pass, target.index, target.target, id == history.description.previous,
                            historyImageView(imageFormat(image->format).native, target.range, type)
                        });
                    }
                }
            }
        }

        bool validTransferTables(const RecordingProgram& recording) noexcept
        {
            const auto& program = recording.transfers;
            std::size_t next_instruction = 0, buffer_copy = 0, image_copy = 0, image_buffer = 0, fill = 0, clear = 0;
            const auto consume = [](const auto& column, std::size_t& cursor, std::uint32_t row) noexcept
            {
                if (row != cursor || cursor >= column.size()) return false;
                ++cursor;
                return true;
            };
            for (const auto& pass : recording.transfer)
            {
                const bool ordered = pass.first == next_instruction && pass.first_clear == clear;
                if (!ordered || pass.count > program.instructions.size() - next_instruction) return false;
                for (const auto [opcode, row] : program.commands(pass))
                {
                    switch (opcode)
                    {
                    case ETransferOpcode::COPY_BUFFER:
                        if (!consume(program.buffer_copies, buffer_copy, row)) return false;
                        break;
                    case ETransferOpcode::COPY_IMAGE:
                        if (!consume(program.image_copies, image_copy, row)) return false;
                        break;
                    case ETransferOpcode::UPLOAD:
                    case ETransferOpcode::READBACK:
                        if (!consume(program.image_buffers, image_buffer, row)) return false;
                        break;
                    case ETransferOpcode::ZERO_BUFFER:
                    case ETransferOpcode::FILL_STORAGE:
                        if (!consume(program.fills, fill, row)) return false;
                        break;
                    case ETransferOpcode::CLEAR_COLOR:
                    case ETransferOpcode::CLEAR_DEPTH:
                        if (!consume(program.clears, clear, row) || program.clears[row].view != row - pass.first_clear)
                            return false;
                        break;
                    }
                }
                if (clear - pass.first_clear != pass.clear_view_count) return false;
                next_instruction += pass.count;
            }
            const auto dynamic_dispatches = std::ranges::count_if(recording.compute, [](const auto& command)
            {
                return !std::holds_alternative<nri::DispatchDesc>(command.dispatch);
            });
            const auto transfer_fields = program.instructions.size() + program.buffer_copies.size();
            if (transfer_fields > UINT32_MAX || std::size_t(dynamic_dispatches) > UINT32_MAX - transfer_fields)
                return false;
            return next_instruction == program.instructions.size() && buffer_copy == program.buffer_copies.size() &&
                image_copy == program.image_copies.size() && image_buffer == program.image_buffers.size() &&
                fill == program.fills.size() && clear == program.clears.size();
        }

    } // namespace

    bool GraphCompiler::Workspace::validRecordingTables(const compiled::RenderGraph& plan) const noexcept
    {
        const auto& program = plan.recording_;
        const auto& batches = plan.submissions_;
        const auto resource_count = plan.resources_.size();
        const auto parameter_slots = bindings.passes.size();
        const auto condition_slots = plan.condition_count_;
        const auto history_slots = plan.histories_.size();
        const auto pass_count = program.passes.size();
        const auto inBounds = [pass_count](const auto& range) noexcept
        {
            return range.first_pass <= pass_count && range.pass_count <= pass_count - range.first_pass;
        };
        const auto covers = [](const auto& range, std::size_t pass) noexcept
        {
            return range.first_pass <= pass && pass - range.first_pass < range.pass_count;
        };
        const auto validTransitions = [resource_count](const BarrierBatch& barriers, std::size_t before) noexcept
        {
            const auto valid = [=](const auto& barrier) noexcept
            {
                const bool valid_predecessor = barrier.previous_slot == kGraphInvalidIndex ||
                    barrier.previous_slot < before;
                return barrier.resource < resource_count && valid_predecessor;
            };
            return std::ranges::all_of(barriers.images, valid) && std::ranges::all_of(barriers.buffers, valid);
        };
        const auto validCondition = [&](const ConditionalRegion& region) noexcept
        {
            const auto slots = region.source == EConditionSource::HISTORY_VALID ? history_slots :
                region.source == EConditionSource::SAME_BINDING ? plan.binding_predicate_count_ : condition_slots;
            return inBounds(region) && region.pass_count != 0 && region.input_slot < slots;
        };
        const bool valid_intervals = std::ranges::all_of(program.units, inBounds) &&
            std::ranges::all_of(program.groups, inBounds) &&
            std::ranges::all_of(program.conditions, validCondition);
        if (!valid_intervals || batches.empty() || !validTransitions(program.after, pass_count))
        {
            return false;
        }
        std::size_t first_pass = 0;
        for (std::size_t slot = 0; slot < batches.size(); ++slot)
        {
            const auto& batch = batches[slot];
            const bool valid_dependencies = std::ranges::all_of(batch.dependencies, [&](const auto& dependency)
            {
                return dependency.producer < slot;
            });
            const bool valid_output = batch.output_begin == kGraphInvalidIndex ||
                (batch.output_begin >= first_pass && batch.output_begin - first_pass <= batch.pass_count);
            const auto& writes = plan.output_write_regions_;
            const bool valid_output_regions = batch.first_output_region <= writes.size() &&
                batch.output_region_count <= writes.size() - batch.first_output_region &&
                std::ranges::all_of(std::span(writes).subspan(batch.first_output_region, batch.output_region_count),
                    [&](auto region) { return region < program.conditions.size(); });
            if (!inBounds(batch) || batch.first_pass != first_pass || !valid_dependencies ||
                !valid_output || !valid_output_regions)
            {
                return false;
            }
            first_pass += batch.pass_count;
        }
        if (first_pass != pass_count)
        {
            return false;
        }
        constexpr auto no_slot = kGraphInvalidIndex;
        const std::array packet_counts{program.raster.size(), program.compute.size(), program.transfer.size()};
        if (program.raster_scopes.size() != program.raster.size()) return false;
        std::array<std::size_t, 3> next_packet{};
        for (std::size_t slot = 0; slot < pass_count; ++slot)
        {
            const auto& pass = program.passes[slot];
            const bool valid_packet = std::visit([&](auto packet) noexcept
            {
                const auto kind = static_cast<std::size_t>(packet.kind);
                return packet.index == next_packet[kind]++ && packet.index < packet_counts[kind];
            }, pass.commands);
            const bool valid_unit = pass.unit < program.units.size() && covers(program.units[pass.unit], slot);
            const bool valid_submit = pass.submission < batches.size() && covers(batches[pass.submission], slot) &&
                batches[pass.submission].queue == pass.queue;
            const bool valid_group = std::holds_alternative<RasterCommandSlot>(pass.commands) ?
                (pass.rendering_group < program.groups.size() &&
                 covers(program.groups[pass.rendering_group], slot)) : pass.rendering_group == no_slot;
            const bool valid_condition = pass.condition_region == no_slot ||
                (pass.condition_region < program.conditions.size() &&
                 covers(program.conditions[pass.condition_region], slot));
            const bool valid_pass = valid_packet && valid_unit && valid_submit && valid_group && valid_condition &&
                pass.binding_slot < parameter_slots && validTransitions(pass.before, slot);
            if (!valid_pass)
            {
                return false;
            }
        }
        return next_packet == packet_counts;
    }

    RenderResult<compiled::RenderGraph>
    GraphCompiler::Workspace::planStructure(const EffectOutputDesc* target) noexcept
    {
        const auto& graph = source.graph;
        const auto& analysis = source.analysis;
        const auto& [limits, queues, device_addresses] = device;
        if (!queues[0] || !queues[1] || !queues[2] || queues[0]->type != nri::QueueType::GRAPHICS)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        if (target)
        {
            if (auto valid = validateEffectOutput(*target); !valid)
            {
                return cxx::unexpected(valid.error());
            }
        }
        const bool is_invalid_address_mode =
            device_addresses && (limits.graphicsAPI != nri::GraphicsAPI::VK || !limits.shaderFeatures.nativeI64);
        if (is_invalid_address_mode)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        compiled::RenderGraph plan(target ? *target : EffectOutputDesc{});
        plan.device_addresses_ = device_addresses;
        plan.condition_count_ = static_cast<std::uint32_t>(graph.conditions().size());
        plan.binding_predicate_count_ = static_cast<std::uint32_t>(graph.bindingPredicates().size());
        std::vector<bool> live(graph.resources().size(), false);
        for (const auto& lifetime : analysis.lifetimes)
        {
            live[lifetime.version.resource.index] = true;
        }
        for (std::uint32_t i = 0; i < graph.resources().size(); ++i)
        {
            const auto& resource = graph.resources()[i];
            plan.resources_.push_back({resource.description, resource.origin});
            auto& output = plan.resources_.back();
            output.used = live[i];
            output.memory_used = live[i];
            if (!live[i])
            {
                continue;
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
            {
                if (image->runtime_layers)
                {
                    if (resource.origin != EGraphOrigin::TRANSIENT || image->extent.source != EImageExtentSource::FIXED)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                    for (const auto& pass : graph.passes())
                    {
                        for (const auto& use : pass.uses)
                        {
                            if (use.version.resource.index == i && !use.whole_resource)
                            {
                                return cxx::unexpected(RenderError{kGraphInvalidRange});
                            }
                        }
                    }
                }
                const auto format = imageFormat(image->format).native;
                const bool invalid = format == nri::Format::UNKNOWN ||
                                     image->width > limits.dimensions.texture2DMaxDim ||
                                     image->height > limits.dimensions.texture2DMaxDim ||
                                     image->layers > limits.dimensions.textureLayerMaxNum;
                if (invalid)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                output.target_sized = (image->extent.source == EImageExtentSource::TARGET);
                if (output.target_sized && !target)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
        }
        for (const auto id : source.concurrent_image_reads)
        {
            if (id.index >= plan.resources_.size()) return cxx::unexpected(RenderError{kGraphUnknownResource});
            auto& imported = plan.resources_[id.index];
            const bool image = std::holds_alternative<GraphImageDesc>(imported.description);
            if (imported.origin != EGraphOrigin::IMPORTED || !image)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            imported.concurrent_image_read = true;
        }
        for (std::uint32_t index = 0; index < plan.resources_.size(); ++index)
        {
            auto& resource = plan.resources_[index];
            resource.sampled_target_input = resource.memory_used && resource.origin == EGraphOrigin::IMPORTED &&
                                            std::holds_alternative<GraphImageDesc>(resource.description) &&
                                            graph.resources()[index].initial_use == EGraphUse::SAMPLED;
        }
        for (auto id : analysis.execution_order)
        {
            for (const auto& use : graph.passes()[id.index].uses)
            {
                if (use.access != EGraphAccess::READ || use.usage != EGraphUse::SAMPLED)
                {
                    plan.resources_[use.version.resource.index].sampled_target_input = false;
                }
            }
        }
        for (const auto& history : graph.histories())
        {
            auto& previous = plan.resources_[history.previous.index];
            auto& current = plan.resources_[history.current.index];
            if (!previous.used && !current.used)
            {
                continue;
            }
            if (!history.publication)
            {
                return cxx::unexpected(RenderError{kGraphMissingInitial});
            }
            const auto index = static_cast<std::uint32_t>(plan.histories_.size());
            previous.history = current.history = index;
            previous.used = current.used = previous.memory_used = current.memory_used = true;
            if (const auto* image = std::get_if<GraphImageDesc>(&current.description))
            {
                previous.target_sized = current.target_sized = (image->extent.source == EImageExtentSource::TARGET);
            }
            plan.histories_.push_back({history});
        }
        for (const auto& resource : plan.resources_)
        {
            const bool unpaired_history =
                resource.memory_used && resource.history == kGraphInvalidIndex &&
                (resource.origin == EGraphOrigin::HISTORY_PREVIOUS || resource.origin == EGraphOrigin::HISTORY_CURRENT);
            if (unpaired_history)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
        }
        for (const auto& output : graph.outputs())
        {
            if (!output.target)
            {
                continue;
            }
            if (!target || plan.output_ != kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const auto index = output.version.resource.index;
            auto& resource = plan.resources_[index];
            const auto* image = std::get_if<GraphImageDesc>(&resource.description);
            const auto& requirement = *output.target;
            const bool relative = requirement.width == 0 && requirement.height == 0;
            const bool incompatible =
                !image || resource.origin != EGraphOrigin::TRANSIENT || requirement.format != target->format ||
                requirement.samples != target->samples ||
                (image && (image->extent.numerator != image->extent.denominator || image->extent.alignment != 1));
            if (incompatible)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            plan.output_ = index;
            plan.fixed_output_extent_ = {requirement.width, requirement.height};
            resource.target_sized = relative;
        }
        auto layouts = planLayouts(limits, graph, analysis, device_addresses);
        if (!layouts)
        {
            return cxx::unexpected(layouts.error());
        }
        auto source_layouts = std::move(layouts->passes);
        layouts->passes.reserve(analysis.execution_order.size());
        for (auto id : analysis.execution_order)
        {
            layouts->passes.push_back(std::move(source_layouts[id.index]));
        }
        bindings = std::move(*layouts);
        std::size_t parameter_bytes = 0;
        for (const auto id : analysis.execution_order)
        {
            const auto& source = graph.passes()[id.index];
            if (source.uses.empty() && !source.side_effect)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            CompiledPass pass;
            pass.binding_slot = static_cast<std::uint32_t>(plan.recording_.passes.size());
            pass.parameter_offset = parameter_bytes;
            pass.parameter_size = bindings.passes[pass.binding_slot].initial_parameters.size();
            parameter_bytes += pass.parameter_size;
            if (source.condition && source.condition->source == EConditionSource::HISTORY_VALID &&
                plan.resources_[source.condition->input].history == kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            plan.recording_.passes.push_back(std::move(pass));
        }
        for (std::uint32_t p = 0; p < plan.recording_.passes.size();)
        {
            const auto& source = graph.passes()[analysis.execution_order[p].index];
            const bool is_sequence = source.sequence_first.index != kGraphInvalidIndex;
            const auto count = is_sequence ? source.sequence_last.index - source.sequence_first.index + 1 : 1;
            const auto unit = static_cast<std::uint32_t>(plan.recording_.units.size());
            for (auto offset = 0u; offset < count; ++offset)
            {
                plan.recording_.passes[p + offset].unit = unit;
            }
            plan.recording_.units.push_back({p, count});
            p += count;
        }
        if (auto lowered = lowerCommands(plan, graph, analysis.execution_order, limits); !lowered)
        {
            return cxx::unexpected(lowered.error());
        }
        return plan;
    }

    RenderResult<void> GraphCompiler::Workspace::finishPlan(compiled::RenderGraph& plan) noexcept
    {
        const auto& graph = source.graph;
        const auto& analysis = source.analysis;
        const auto& queues = device.queues;
        auto& resource_reuse = diagnostics.resource_reuse;
        for (const auto& placement : bindings.passes)
        {
            for (std::size_t i = 0; i < placement.uses.size(); ++i)
            {
                if (placement.bindings[placement.uses[i].field].count == 0)
                {
                    plan.resources_[placement.uses[i].resource].shared_table = true;
                }
            }
        }
        planConditions(graph, analysis.execution_order, plan.resources_, plan.recording_);
        const auto order = std::span(analysis.execution_order);
        collectUseStages();
        const bool presents = plan.output_description_.kind == ETargetKind::SURFACE;
        const auto surface_output = presents ? plan.output_ : kGraphInvalidIndex;
        const auto lowerSchedule = [&]() noexcept -> RenderResult<void>
        {
            // Derive the actual submit DAG from the candidate's logical transitions before choosing storage.
            auto barriers = planBarriers(plan);
            if (!barriers)
            {
                return barriers;
            }
            plan.submissions_.clear();
            planSubmissions(plan);
            plan.physical_resources_ = planPhysicalResources(plan);
            return {};
        };
        if (policy.exercise_async)
        {
            (void)assignAsyncCompute(plan, surface_output);
        }
        if (auto baseline = lowerSchedule(); !baseline)
        {
            return baseline;
        }

        const auto& costs = policy.costs;
        const bool has_calibration = costs.graphics_submit_ns != 0 && costs.compute_submit_ns != 0;
        const bool has_durations = has_calibration && std::ranges::all_of(order, [&](auto id) noexcept
        {
            return graph.passes()[id.index].estimated_gpu_ns != 0;
        });
        const bool compares_async = !policy.exercise_async && has_durations && queues[0] != queues[1];
        if (compares_async && assignAsyncCompute(plan, surface_output))
        {
            const auto baseline_ns = estimateScheduleNs(graph, order, plan.submissions_, costs);
            auto baseline_storage = std::move(plan.physical_resources_);
            const auto candidate = lowerSchedule();
            const bool improves_cost = candidate && estimateScheduleNs(graph, order, plan.submissions_, costs) <
                baseline_ns;
            const bool should_adopt = improves_cost && preservesObjectReuse(baseline_storage, plan.physical_resources_);
            if (!should_adopt)
            {
                for (auto& pass : plan.recording_.passes)
                {
                    pass.queue = EQueueRole::GRAPHICS;
                }
                if (auto restored = lowerSchedule(); !restored)
                {
                    return restored;
                }
            }
        }

        for (auto& history : plan.histories_)
        {
            auto& previous = plan.resources_[history.description.previous.index];
            auto& current = plan.resources_[history.description.current.index];
            if (const auto* image = std::get_if<GraphImageDesc>(&current.description))
            {
                const bool depth = rdesc::textureAspectMask(image->format) != 1;
                const auto initialization =
                    depth ? nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT : nri::TextureUsageBits::COLOR_ATTACHMENT;
                previous.image_usage = current.image_usage =
                    previous.image_usage | current.image_usage | initialization;
            }
            else
            {
                previous.buffer_usage = current.buffer_usage =
                    previous.buffer_usage | current.buffer_usage | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
            }
            auto& sample = history.description;
            if (sample.alternate_sample)
            {
                sample.select_primary.reset();
                sample.alternate_sample.reset();
            }
            for (auto guard : {&sample.publish_when, &sample.select_primary})
            {
                if (!*guard || (*guard)->source != EConditionSource::HISTORY_VALID) continue;
                const auto source_history = plan.resources_[(*guard)->input].history;
                if (source_history == kGraphInvalidIndex) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                (*guard)->input = source_history;
            }
        }
        if (std::ranges::find(resource_reuse, EResourceReuse::REUSED) != resource_reuse.end())
        {
            // Rebuild only the cold transition templates using the selected physical identities.
            // Their ordering is already proved: no submission dependency or CPU wait is added.
            auto barriers = planBarriers(plan, &plan.physical_resources_);
            if (!barriers)
            {
                return cxx::unexpected(barriers.error());
            }
        }
        if (plan.output_first_batch_ != kGraphInvalidIndex)
        {
            const auto& batch = plan.submissions_[plan.output_first_batch_];
            bool found_output_use = false;
            for (auto p = batch.first_pass; p < batch.first_pass + batch.pass_count; ++p)
            {
                for (const auto& [resource, stages] : use_stages[analysis.execution_order[p].index])
                {
                    if (resource != plan.output_)
                    {
                        continue;
                    }
                    plan.output_wait_stages_ = found_output_use ? combineStages(plan.output_wait_stages_, stages) : stages;
                    found_output_use = true;
                }
            }
        }
        auto rendering = planRendering(analysis, plan.resources_, plan.recording_);
        if (!rendering)
        {
            return cxx::unexpected(rendering.error());
        }
        for (std::uint32_t index = 0; index < plan.submissions_.size(); ++index)
        {
            auto& batch = plan.submissions_[index];
            batch.reusable_recording = batch.histories.empty() && (index != 0 || plan.histories_.empty());
            for (auto p = batch.first_pass; p < batch.first_pass + batch.pass_count; ++p)
            {
                const auto& pass = plan.recording_.passes[p];
                const auto& source = graph.passes()[analysis.execution_order[p].index];
                if (source.side_effect)
                {
                    batch.reusable_recording = false;
                }
                if (plan.output_description_.kind == ETargetKind::SURFACE &&
                    std::ranges::any_of(
                        source.uses,
                        [&](const auto& use) noexcept { return use.version.resource.index == plan.output_; }
                    ))
                {
                    batch.output_begin = std::min(batch.output_begin, p);
                }
            }
            if (plan.output_description_.kind == ETargetKind::SURFACE && index + 1 == plan.submissions_.size() &&
                std::ranges::any_of(
                    plan.recording_.after.images,
                    [&](const auto& barrier) noexcept { return barrier.resource == plan.output_; }
                ))
            {
                batch.output_begin = std::min(batch.output_begin, batch.first_pass + batch.pass_count);
            }
            if (batch.output_begin != kGraphInvalidIndex)
            {
                auto begin = batch.output_begin;
                do
                {
                    batch.output_begin = begin;
                    if (begin == batch.first_pass + batch.pass_count)
                    {
                        break;
                    }
                    const auto& pass = plan.recording_.passes[begin];
                    begin = std::min(begin, plan.recording_.units[pass.unit].first_pass);
                    if (pass.rendering_group != kGraphInvalidIndex)
                    {
                        begin = std::min(begin, plan.recording_.groups[pass.rendering_group].first_pass);
                    }
                    if (pass.condition_region != kGraphInvalidIndex)
                    {
                        begin = std::min(begin, plan.recording_.conditions[pass.condition_region].first_pass);
                    }
                } while (begin != batch.output_begin);
            }
        }
        if (allocation == ETransientAllocation::ALIAS_MEMORY) publishAliasFrontiers(plan);
        compactImageTransitions(plan.resources_, plan.recording_);
        if (!validRecordingTables(plan))
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        auto table = std::make_unique<GraphBindingPlan>();
        table->recordings = recordingDependencies(plan);
        table->passes.reserve(bindings.passes.size());
        diagnostics.descriptor_fields.reserve(bindings.passes.size());
        for (auto& source : bindings.passes)
        {
            if (auto added = addPrivateDescriptors(table->private_pool, source); !added)
            {
                return added;
            }
            table->passes.push_back({
                {table->descriptor_fields.size(), source.bindings.size()},
                {table->descriptor_uses.size(), source.uses.size()},
                !source.private_bindings.empty()
            });
            table->descriptor_uses.insert(table->descriptor_uses.end(), source.uses.begin(), source.uses.end());
            table->initial_parameters.insert(
                table->initial_parameters.end(), source.initial_parameters.begin(), source.initial_parameters.end()
            );
            auto& fields = diagnostics.descriptor_fields.emplace_back();
            fields.reserve(source.bindings.size());
            for (auto& binding : source.bindings)
            {
                const auto view = binding.bufferView(nullptr, 0, 0);
                table->descriptor_fields.push_back({
                    view.type, view.format, view.structureStride, binding.image_arrayed, binding.count != 0
                });
                fields.push_back({
                    std::move(binding.name), binding.set, binding.binding, binding.count, binding.stages,
                    binding.block.size, static_cast<std::uint8_t>(binding.kind), binding.block.runtime_records
                });
            }
        }
        linkHistoryViews(plan.histories_, plan, *table);
        plan.binding_plan_ = std::move(table);
        return {};
    }

    RenderResult<compiled::RenderGraph> GraphCompiler::compileForRecordingTest(
        GraphDeviceFacts device, AnalyzedGraph source, ETransientAllocation allocation, GraphQueuePolicy policy
    ) noexcept
    {
        Workspace workspace{source, device, allocation, policy};
        auto plan = workspace.planStructure(nullptr);
        if (!plan) return cxx::unexpected(plan.error());
        auto finished = workspace.finishPlan(*plan);
        if (!finished)
        {
            return cxx::unexpected(finished.error());
        }
        return plan;
    }

    RenderResult<RasterProgramLink>
    linkRasterStreamProgram(const DeviceSession& session, RasterProgramRecipe recipe) noexcept
    {
        const auto& [arguments, parameters, target, artifact, graphics, geometry_locations] = recipe;
        if (artifact.target != session.enabledFeatures().shader_target)
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        auto layout = planRasterLayout(session.description(), artifact.interface, arguments, parameters);
        if (!layout) return cxx::unexpected(layout.error());
        std::uint32_t shared_texture_stages = 0;
        std::uint32_t record_stages = 0;
        rdesc::ShaderBlock record_layout;
        // Layout planning already resolved each reflected field to its declared argument.
        for (const auto& use : layout->uses)
        {
            if (use.element != 0) continue;
            const auto& field = layout->bindings[use.field];
            const auto index = field.count && field.kind == EDescriptorKind::SAMPLER ? use.sampler : use.resource;
            const auto& argument = arguments[index];
            const bool supported_kind = field.kind == EDescriptorKind::UNIFORM_BUFFER ||
                field.kind == EDescriptorKind::STORAGE_BUFFER || field.kind == EDescriptorKind::SAMPLED_IMAGE ||
                field.kind == EDescriptorKind::SAMPLER;
            if (!supported_kind) return cxx::unexpected(RenderError{kRuntimeUnsupported});
            if (field.count != 0)
            {
                if (argument.shared_records) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                continue;
            }

            const bool shared_image = field.kind == EDescriptorKind::SAMPLED_IMAGE && !field.image_arrayed &&
                !field.image_depth;
            const bool shared_sampler = field.kind == EDescriptorKind::SAMPLER && !argument.comparison_sampler;
            const bool structured = field.block.runtime_records && field.block.hasStructuredLayout();
            const bool shared_records = argument.shared_records && field.kind == EDescriptorKind::STORAGE_BUFFER &&
                structured && target.shared_records && !record_stages;
            if (shared_records)
            {
                record_stages = field.stages;
                record_layout = field.block;
                continue;
            }
            if (!target.shared_textures || (!shared_image && !shared_sampler))
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            if (shared_image) shared_texture_stages |= field.stages;
        }
        // Shared descriptors are supplied by the device tables. Each draw provides private bindings only.
        std::erase_if(layout->uses, [&](const auto& use) noexcept { return layout->bindings[use.field].count == 0; });

        std::vector<CompiledRasterStream::ArgumentUses> argument_to_use(arguments.size());
        for (std::uint32_t i = 0; i < layout->uses.size(); ++i)
        {
            const auto& use = layout->uses[i];
            const auto slot = use.kind == EDescriptorKind::SAMPLER ? use.sampler : use.resource;
            if (slot >= argument_to_use.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            auto& range = argument_to_use[slot];
            if (!range.count) range.first = i;
            const bool consecutive = i - range.first == range.count && use.element == range.count;
            if (!consecutive) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            ++range.count;
        }

        RasterDescriptorLayout descriptors;
        nri::DescriptorPoolDesc pool{};
        if (auto counted = addPrivateDescriptors(pool, *layout); !counted) return cxx::unexpected(counted.error());
        for (std::size_t i = 0; i < kDescriptorPoolCounts.size(); ++i)
            descriptors.per_set[i] = pool.*kDescriptorPoolCounts[i];
        descriptors.parameter_bytes = layout->parameter_bytes;
        descriptors.fields.reserve(layout->uses.size());
        for (const auto& use : layout->uses)
        {
            const auto& field = layout->bindings[use.field];
            const auto view = field.bufferView(nullptr, 0, 0);
            descriptors.fields.push_back({
                use.binding, use.element, field.block.size, use.kind,
                {view.type, view.format, view.structureStride, field.image_arrayed, true},
                field.image_depth, use.comparison_sampler
            });
        }

        auto geometry_attributes = graphics.vertex_attributes;
        if (!geometry_locations.empty())
        {
            if (geometry_locations.size() != geometry_attributes.size())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            for (std::size_t i = 0; i < geometry_attributes.size(); ++i)
                geometry_attributes[i].location = geometry_locations[i];
        }
        RasterProgramKey key{graphics, target.colors, target.depth, target.samples};
        if (target.dynamic_depth_bias) key.state.depth_bias_constant = 1;
        return RasterProgramLink{
            {artifact, std::move(*layout), std::move(key)}, std::move(descriptors), std::move(argument_to_use),
            graphics.vertex_bindings, std::move(geometry_attributes), shared_texture_stages, record_stages,
            std::move(record_layout)
        };
    }

    CompiledRasterStream::Program RasterProgramLink::finish(PreparedProgram compiled) && noexcept
    {
        descriptors.native = compiled.bindings.recordBindings();
        return CompiledRasterStream::Program{
            compiled.pipeline, compiled.id, std::move(descriptors), std::move(argument_to_use), std::move(vertices),
            std::move(geometry_attributes), shared_texture_stages, record_stages, std::move(record_layout)
        };
    }

    exec::task<RenderResult<void>>
    GraphCompiler::Workspace::prepareOperations(
        DeviceSession& session, PreparedRenderGraph& candidate, execution::CpuScheduler cpu
    ) noexcept
    {
        const auto& plan = candidate.graph;
        auto& native = candidate.graph.programs_;
        native.slots_.resize(plan.passes().size());
        native.streams_.resize(plan.passes().size());
        native.owner_ = &session;
        const auto& graph = source.graph;
        const auto order = std::span(source.analysis.execution_order);
        auto& layouts = bindings;
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& program_cache = session.programs();
        native.samplers_.reserve(bindings.sampler_inputs.size());
        for (auto input : bindings.sampler_inputs)
        {
            if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kCommandCancelled});
            auto descriptor = session.descriptors().sampler(graph.samplers()[input]);
            if (!descriptor) co_return cxx::unexpected(descriptor.error());
            native.samplers_.push_back(*descriptor);
        }
        for (auto& pass : plan.recording_.passes)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kCommandCancelled});
            }
            const auto& source = graph.passes()[order[pass.binding_slot].index];
            const auto* shader_input = source.bindings().shader;
            auto& bindings = layouts.passes[pass.binding_slot];
            const rdesc::ShaderProgramArtifact* shader = nullptr;
            std::vector<nri::Format> colors;
            std::optional<PassRenderingInfo> rendering;
            if (const auto* scope = plan.recording_.rasterScope(pass))
            {
                rendering.emplace();
                rendering->dynamic_depth_bias = scope->section_depth_bias;
                for (const auto& color : scope->colors)
                {
                    const auto& image =
                        std::get<GraphImageDesc>(plan.resources_[color.access.version.resource.index].description);
                    colors.push_back(imageFormat(image.format).native);
                    rendering->samples = static_cast<std::uint8_t>(image.samples);
                }
                if (scope->depth)
                {
                    const auto resource = scope->depth->access.version.resource.index;
                    const auto& image = std::get<GraphImageDesc>(plan.resources_[resource].description);
                    rendering->depth = imageFormat(image.format).native;
                    rendering->samples = static_cast<std::uint8_t>(image.samples);
                }
            }
            if (shader_input)
            {
                const auto& declaration = graph.programs()[shader_input->program];
                shader = declaration.find(session.enabledFeatures().shader_target, plan.device_addresses_);
                if (!shader)
                {
                    co_return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                std::optional<RasterProgramKey> raster;
                if (rendering)
                {
                    auto& key = raster.emplace();
                    key.state = std::get<RasterWork>(source.command).fixed().graphics;
                    if (rendering->dynamic_depth_bias) key.state.depth_bias_constant = 1;
                    key.colors = colors;
                    key.depth = rendering->depth;
                    key.samples = rendering->samples;
                }
                auto prepared = co_await program_cache.prepareEffectProgram(
                    {*shader, std::move(bindings), std::move(raster)}, cpu
                );
                if (!prepared) co_return cxx::unexpected(prepared.error());
                bindings = std::move(prepared->bindings);
                native.slots_[pass.binding_slot].program = prepared->id;
                native.slots_[pass.binding_slot].pipeline = prepared->pipeline;
            }
            else if (!bindings.private_bindings.empty())
            {
                auto layout = program_cache.acquireLayout(bindings, static_cast<std::uint32_t>(colors.size()));
                if (!layout)
                {
                    co_return cxx::unexpected(layout.error());
                }
                native.slots_[pass.binding_slot].layout = CompiledPrograms::identity(**layout);
            }
            const auto* raster = std::get_if<RasterWork>(&source.command);
            const auto* description = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
            if (description && description->usesStreamProducer())
            {
                auto& stream = native.streams_[pass.binding_slot];
                stream = std::make_unique<CompiledRasterStream>();
                stream->list = std::get<RasterListCommands>(plan.rasterCommands(pass)->draw).data_index;
                for (const auto& resource : description->resources)
                {
                    if (const auto* range = std::get_if<GraphBufferRange>(&resource.range))
                    {
                        const auto index = resource.version.resource.index;
                        const bool dynamic_extent = resource.whole_resource &&
                            plan.resources_[index].origin == EGraphOrigin::FRAME_BUFFER;
                        stream->buffers.push_back({index, *range, resource.usage, resource.whole_resource, dynamic_extent});
                        continue;
                    }
                    if (resource.usage != EGraphUse::SAMPLED) continue;
                    const auto id = resource.version.resource.index;
                    const auto& image = std::get<GraphImageDesc>(plan.resources_[id].description);
                    if (image.samples != 1) co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    const auto fallback = resource.fallback ? resource.fallback->resource.index : kGraphInvalidIndex;
                    stream->images.push_back({id, std::get<GraphImageRange>(resource.range), fallback});
                }
                std::ranges::stable_sort(stream->buffers, {}, &CompiledRasterStream::BufferAccess::resource);
                std::ranges::stable_sort(stream->images, {}, &CompiledRasterStream::ImageAccess::resource);
                for (const auto sampler : description->stream_samplers)
                {
                    const auto& authored = graph.samplers()[sampler];
                    auto descriptor = session.descriptors().sampler(authored);
                    if (!descriptor) co_return cxx::unexpected(descriptor.error());
                    stream->samplers.push_back({*descriptor, authored.comparison.has_value()});
                }
                for (const auto& input : description->inputs) stream->argument_uses.push_back(input.usage);
                const RasterStreamTarget target{
                    colors, rendering->depth, rendering->samples, rendering->dynamic_depth_bias,
                    description->requiresCapability(EStorageCapability::SAMPLED_IMAGE_ARRAY),
                    description->requiresCapability(EStorageCapability::STORAGE_BUFFER_ARRAY)
                };
                if (description->runtime_stream_programs)
                    stream->runtime = CompiledRasterStream::RuntimeContract{description->inputs, target};
                for (const auto& authored : description->stream_programs)
                {
                    const auto* artifact = graph.programs()[authored.program].find(
                        session.enabledFeatures().shader_target, plan.device_addresses_
                    );
                    if (!artifact) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
                    auto link = linkRasterStreamProgram(
                        session,
                        {description->inputs, authored.parameters, target, *artifact, authored.graphics,
                         authored.geometry_locations}
                    );
                    if (!link) co_return cxx::unexpected(link.error());
                    auto program = co_await program_cache.prepareEffectProgram(std::move(link->input), cpu);
                    if (!program) co_return cxx::unexpected(program.error());
                    stream->programs.push_back(std::move(*link).finish(std::move(*program)));
                }
            }
            native.slots_[pass.binding_slot].descriptors = bindings.recordBindings();
            bindings.layout = nullptr;
        }
        co_return RenderResult<void>{};
    }

    RenderResult<void>
    GraphCompiler::lowerCommands(
        compiled::RenderGraph& plan, const RenderGraph& graph, std::span<const GraphPassId> order,
        const nri::DeviceDesc& limits
    ) noexcept
    {
        const auto target = limits.graphicsAPI == nri::GraphicsAPI::D3D12
            ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
        std::uint32_t raster_lists = 0;
        std::vector<std::uint8_t> materialized;
        materialized.reserve(plan.resources_.size());
        for (const auto& resource : plan.resources_) materialized.push_back(resource.memory_used);
        for (auto& pass : plan.recording_.passes)
        {
            const auto& source = graph.passes()[order[pass.binding_slot].index];
            const auto* shader_input = source.bindings().shader;
            const rdesc::ShaderInfo* shader_info = nullptr;
            if (shader_input)
            {
                const auto* shader = graph.programs()[shader_input->program].find(target, plan.device_addresses_);
                if (!shader)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                shader_info = &shader->interface;
            }
            PassCompileContext context{
                limits, shader_info, graph.resources(),
                &raster_lists, materialized
            };
            auto& recording = plan.recording_;
            auto commands = std::visit(
                [&](const auto& command) noexcept -> RenderResult<VCommandSlot>
                {
                    using Command = std::remove_cvref_t<decltype(command)>;
                    if constexpr (std::same_as<Command, RasterWork>)
                    {
                        if (auto valid = validateRasterScope(command, graph, limits); !valid)
                            return cxx::unexpected(valid.error());
                        const auto* fixed = std::get_if<RasterWork::FixedProgram>(&command.program);
                        const auto* stream = std::get_if<RasterPassTemplate>(&command.program);
                        const bool section_bias = stream && stream->section_depth_bias;
                        if (section_bias && !limits.features.dynamicDepthBias)
                            return cxx::unexpected(RenderError{kDeviceUnsupported, {order[pass.binding_slot].index}});
                        auto packet = RasterPass::compile(command, context);
                        if (!packet) return cxx::unexpected(packet.error());
                        const RasterCommandSlot slot{static_cast<std::uint32_t>(recording.raster.size())};
                        auto& scope = recording.raster_scopes.emplace_back(command, !fixed);
                        scope.section_depth_bias = section_bias;
                        if (fixed)
                            scope.depth_bias = {fixed->graphics.depth_bias_constant, 0, fixed->graphics.depth_bias_slope};
                        recording.raster.push_back(std::move(*packet));
                        return VCommandSlot{slot};
                    }
                    else if constexpr (std::same_as<Command, ComputeWork>)
                    {
                        auto packet = ComputePass::compile(command, context);
                        if (!packet) return cxx::unexpected(packet.error());
                        const ComputeCommandSlot slot{static_cast<std::uint32_t>(recording.compute.size())};
                        recording.compute.push_back(std::move(*packet));
                        return VCommandSlot{slot};
                    }
                    else
                    {
                        auto packet = TransferPass::compile(command.work, command.typed_buffer_writes, context, recording.transfers);
                        if (!packet) return cxx::unexpected(packet.error());
                        const TransferCommandSlot slot{static_cast<std::uint32_t>(recording.transfer.size())};
                        recording.transfer.push_back(std::move(*packet));
                        return VCommandSlot{slot};
                    }
                },
                source.command
            );
            if (!commands)
            {
                return cxx::unexpected(commands.error());
            }
            pass.commands = std::move(*commands);
        }
        if (!validTransferTables(plan.recording_)) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        return {};
    }

    RenderResult<void> GraphCompiler::Workspace::finishDevicePlan(PreparedRenderGraph& candidate) noexcept
    {
        if (auto result = finishPlan(candidate.graph); !result) return result;
        compileFixedBindings(candidate.graph);
        const auto& graph = source.graph;
        const auto& analysis = source.analysis;
        diagnostics.source_passes = analysis.execution_order;
        diagnostics.resource_keys.reserve(graph.resources().size());
        for (const auto& resource : graph.resources())
        {
            diagnostics.resource_keys.push_back(resource.key);
        }
        candidate.diagnostics = std::move(diagnostics);
        return {};
    }

    exec::task<RenderResult<PreparedRenderGraph>>
    GraphCompiler::compile(
        DeviceSession& session, execution::CpuScheduler cpu, AnalyzedGraph source, GraphCompileOptions options
    ) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        const std::array queues{
            &session.queue(EQueueRole::GRAPHICS).properties(), &session.queue(EQueueRole::COMPUTE).properties(),
            &session.queue(EQueueRole::TRANSFER).properties()
        };
        std::optional<Workspace> workspace;
        auto structure = co_await (
            stdexec::schedule(cpu) |
            stdexec::then(
                [&]() noexcept
                {
                    // Query driver capabilities on the compile worker, then keep a snapshot for all planning phases.
                    workspace.emplace(
                        source, GraphDeviceFacts{session.description(), queues, session.usesDeviceAddresses()},
                        options.allocation, GraphQueuePolicy{session.graphQueueCosts(), options.exercise_async}
                    );
                    return workspace->planStructure(options.target ? &*options.target : nullptr);
                }
            ) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<compiled::RenderGraph> { return cxx::unexpected(RenderError{kCpuStopped}); }
            ) |
            stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<compiled::RenderGraph>{})
        );
        if (!structure)
        {
            co_return cxx::unexpected(structure.error());
        }
        PreparedRenderGraph candidate{std::move(*structure), {}};
        auto prepared = co_await workspace->prepareOperations(session, candidate, cpu);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto planned = co_await (
            stdexec::schedule(cpu) |
            stdexec::then(
                [&]() noexcept
                {
                    return workspace->finishDevicePlan(candidate);
                }
            ) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCpuStopped}); }
            ) |
            stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<void>{})
        );
        if (!planned)
        {
            co_return cxx::unexpected(planned.error());
        }
        co_return candidate;
    }
} // namespace lux::render::detail

namespace lux::render::compiled
{
    RenderGraph::RenderGraph(EffectOutputDesc output) noexcept
        : output_description_(output)
    {
    }

    RenderGraph::~RenderGraph() noexcept = default;

    RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;

    const detail::RecordingDependencies& RenderGraph::recordingDependencies() const & noexcept
    {
        return binding_plan_->recordings;
    }

    std::span<const detail::DescriptorAccess> RenderGraph::descriptorUses(std::uint32_t binding_slot) const & noexcept
    {
        return binding_plan_->pass(binding_slot).uses;
    }

} // namespace lux::render::compiled
