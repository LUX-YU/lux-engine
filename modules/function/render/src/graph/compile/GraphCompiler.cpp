#include <exec/variant_sender.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
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
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
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
        bool validRecordingTables(
            const RecordingProgram& program, std::span<const SubmissionBatch> batches,
            std::size_t resource_count, std::size_t parameter_slots, std::size_t condition_slots,
            std::size_t history_slots
        ) noexcept
        {
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
                const auto slots = region.source == EConditionSource::INPUT ? condition_slots : history_slots;
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
                if (!inBounds(batch) || batch.first_pass != first_pass || !valid_dependencies || !valid_output)
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
            for (std::size_t slot = 0; slot < pass_count; ++slot)
            {
                const auto& pass = program.passes[slot];
                const auto kernel = static_cast<std::size_t>(pass.commands.kernel);
                const bool valid_packet = kernel < packet_counts.size() && pass.commands.packet < packet_counts[kernel];
                const bool valid_unit = pass.unit < program.units.size() && covers(program.units[pass.unit], slot);
                const bool valid_submit = pass.submission < batches.size() && covers(batches[pass.submission], slot) &&
                    batches[pass.submission].queue == pass.queue;
                const bool valid_group = pass.rendering_group == no_slot ||
                    (pass.rendering_group < program.groups.size() &&
                     covers(program.groups[pass.rendering_group], slot));
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
            return true;
        }

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

    } // namespace

    RenderResult<RenderGraphExecutable> GraphCompiler::planStructure(
        nri::Device* device, const nri::DeviceDesc& limits, std::array<const QueueProperties*, 3> queues,
        const RenderGraph& graph, const RenderGraphAnalysis& analysis, const EffectOutputDesc* target,
        ETransientAllocation allocation,
        bool device_addresses,
        ShaderBindingPlan& bindings
    ) noexcept
    {
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
        RenderGraphExecutable plan(device, target ? *target : EffectOutputDesc{});
        plan.device_addresses_ = device_addresses;
        plan.condition_count_ = static_cast<std::uint32_t>(graph.conditions().size());
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
            pass.kind = source.kind;
            pass.binding_slot = static_cast<std::uint32_t>(plan.recording_.passes.size());
            pass.parameter_offset = parameter_bytes;
            pass.parameter_size = bindings.passes[pass.binding_slot].initial_parameters.size();
            parameter_bytes += pass.parameter_size;
            if (source.condition && source.condition->source == EConditionSource::HISTORY_VALID &&
                plan.resources_[source.condition->input].history == kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            if (const auto* scope = source.rasterScope())
            {
                if (auto valid = validateRasterScope(*scope, graph, limits); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
                const auto& raster = std::get<RasterWork>(source.command);
                const auto* fixed = std::get_if<RasterWork::FixedProgram>(&raster.program);
                const auto* stream = std::get_if<RasterPassTemplate>(&raster.program);
                const bool section_depth_bias = stream && stream->section_depth_bias;
                if (section_depth_bias && !limits.features.dynamicDepthBias)
                    return cxx::unexpected(RenderError{kDeviceUnsupported, {id.index}});
                const bool owns_draw_sections = !fixed;
                pass.rendering = CompiledRasterScope{*scope, owns_draw_sections};
                pass.rendering->section_depth_bias = section_depth_bias;
            }
            if (source.graphics && pass.rendering)
            {
                pass.rendering->depth_bias = {source.graphics->depth_bias_constant, source.graphics->depth_bias_slope};
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

    RenderResult<void> GraphCompiler::finishPlan(
        RenderGraphExecutable& plan, const RenderGraph& graph, const RenderGraphAnalysis& analysis,
        std::array<const QueueProperties*, 3> queues, const nri::DeviceDesc& limits, ETransientAllocation allocation,
        GraphCompileDiagnostics& diagnostics, GraphQueuePolicy policy, ShaderBindingPlan& parameter_plan
    ) noexcept
    {
        auto& resource_reuse = diagnostics.resource_reuse;
        for (const auto& placement : parameter_plan.passes)
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
        const auto use_stages = collectUseStages(graph, order, parameter_plan, limits.graphicsAPI);
        const bool presents = plan.output_description_.kind == ETargetKind::SURFACE;
        const auto surface_output = presents ? plan.output_ : kGraphInvalidIndex;
        const auto lowerSchedule = [&]() noexcept -> RenderResult<void>
        {
            // Derive the actual submit DAG from the candidate's logical transitions before choosing storage.
            auto barriers = planBarriers(
                graph, analysis, parameter_plan, plan.resources_, plan.recording_, plan.output_description_,
                limits.graphicsAPI
            );
            if (!barriers)
            {
                return barriers;
            }
            plan.submissions_.clear();
            plan.imported_accesses_.clear();
            planSubmissions(
                graph, analysis, plan.resources_, plan.recording_, use_stages, queues, plan.submissions_,
                plan.imported_accesses_, plan.output_, presents
            );
            plan.physical_resources_ = planPhysicalResources(
                graph, order, plan.resources_, plan.recording_, plan.submissions_, queues, allocation, resource_reuse
            );
            return {};
        };
        if (policy.exercise_async)
        {
            (void)assignAsyncCompute(graph, order, queues, surface_output, plan.recording_);
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
        if (compares_async && assignAsyncCompute(graph, order, queues, surface_output, plan.recording_))
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
            for (std::uint32_t p = 0; p < analysis.execution_order.size(); ++p)
            {
                const auto& pass = graph.passes()[analysis.execution_order[p].index];
                if (std::ranges::any_of(
                        pass.uses,
                        [&](const auto& use)
                        {
                            return use.version.resource == history.description.current &&
                                   use.access != EGraphAccess::READ;
                        }
                    ))
                {
                    history.writers.push_back(p);
                }
            }
        }
        for (const auto& access : plan.imported_accesses_)
        {
            if (access.resource.index == plan.output_)
            {
                plan.output_first_batch_ = std::min(plan.output_first_batch_, access.batch);
            }
        }
        if (std::ranges::find(resource_reuse, EResourceReuse::REUSED) != resource_reuse.end())
        {
            // Rebuild only the cold transition templates using the selected physical identities.
            // Their ordering is already proved: no submission dependency or CPU wait is added.
            auto barriers = planBarriers(
                graph, analysis, parameter_plan, plan.resources_, plan.recording_, plan.output_description_,
                limits.graphicsAPI, &plan.physical_resources_
            );
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
                    const bool includes_all = found_output_use && (plan.output_wait_stages_ == nri::StageBits::ALL ||
                                                                   stages == nri::StageBits::ALL);
                    plan.output_wait_stages_ = !found_output_use ? stages
                                               : includes_all    ? nri::StageBits::ALL
                                                                 : plan.output_wait_stages_ | stages;
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
        compactImageTransitions(plan.resources_, plan.recording_);
        const bool valid_program = validRecordingTables(
            plan.recording_, plan.submissions_, plan.resources_.size(), parameter_plan.passes.size(),
            plan.condition_count_, plan.histories_.size()
        );
        if (!valid_program)
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        auto bindings = std::make_unique<GraphBindingPlan>();
        bindings->passes.reserve(parameter_plan.passes.size());
        diagnostics.descriptor_fields.reserve(parameter_plan.passes.size());
        for (auto& source : parameter_plan.passes)
        {
            if (auto added = addPrivateDescriptors(bindings->private_pool, source); !added)
            {
                return added;
            }
            bindings->passes.push_back({
                {bindings->descriptor_fields.size(), source.bindings.size()},
                {bindings->descriptor_uses.size(), source.uses.size()},
                {bindings->initial_parameters.size(), source.initial_parameters.size()},
                !source.private_bindings.empty()
            });
            bindings->descriptor_uses.insert(bindings->descriptor_uses.end(), source.uses.begin(), source.uses.end());
            bindings->initial_parameters.insert(
                bindings->initial_parameters.end(), source.initial_parameters.begin(), source.initial_parameters.end()
            );
            auto& fields = diagnostics.descriptor_fields.emplace_back();
            fields.reserve(source.bindings.size());
            for (auto& binding : source.bindings)
            {
                const auto view = binding.bufferView(nullptr, 0, 0);
                bindings->descriptor_fields.push_back({
                    view.type, view.format, view.structureStride, binding.image_arrayed, binding.count != 0
                });
                fields.push_back({
                    std::move(binding.name), binding.set, binding.binding, binding.count, binding.stages,
                    binding.block.size, static_cast<std::uint8_t>(binding.kind), binding.block.runtime_records
                });
            }
        }
        plan.binding_plan_ = std::move(bindings);
        return {};
    }

    RenderResult<RenderGraphExecutable> GraphCompiler::compileForRecordingTest(
        const nri::DeviceDesc& limits, std::array<const QueueProperties*, 3> queues, const RenderGraph& graph,
        const RenderGraphAnalysis& analysis, ETransientAllocation allocation, GraphQueuePolicy policy
    ) noexcept
    {
        ShaderBindingPlan bindings;
        auto plan = planStructure(
            nullptr, limits, queues, graph, analysis, nullptr, allocation, false, bindings
        );
        if (!plan)
        {
            return cxx::unexpected(plan.error());
        }
        GraphCompileDiagnostics diagnostics;
        auto finished = finishPlan(
            *plan, graph, analysis, queues, limits, allocation, diagnostics, policy, bindings
        );
        if (!finished)
        {
            return cxx::unexpected(finished.error());
        }
        return plan;
    }

    static exec::task<RenderResult<CompiledPrograms::Program*>> prepareGraphProgram(
        RasterProgramKey key, const rdesc::ShaderProgramArtifact& shader, PassBindings& bindings,
        DeviceSession& session, execution::CpuScheduler cpu, bool is_raster,
        std::optional<cxx::function_ref<bool() noexcept>> runtime_current = {}
    ) noexcept
    {
        const auto owner_scheduler = co_await stdexec::read_env(stdexec::get_scheduler);
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& program_cache = session.programs();
        auto relocated = co_await (
            stdexec::schedule(cpu) |
            stdexec::then([&]() noexcept { return relocateShader(shader, bindings.bindings); }) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<rdesc::ShaderProgramArtifact>
                { return cxx::unexpected(RenderError{kCpuStopped}); }
            ) |
            stdexec::continues_on(owner_scheduler) |
            stdexec::upon_error(NativeDeliveryFailure<rdesc::ShaderProgramArtifact>{})
        );
        if (!relocated)
        {
            co_return cxx::unexpected(relocated.error());
        }
        if (runtime_current && !(*runtime_current)()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        auto& retained_shader = program_cache.acquireShader(std::move(*relocated));
        cxx::scope_exit release_shader([&]() noexcept { program_cache.releaseShader(retained_shader); });
        const auto& artifact = program_cache.artifact(retained_shader);
        auto layout = program_cache.acquireLayout(bindings, static_cast<std::uint32_t>(key.colors.size()));
        if (!layout)
        {
            co_return cxx::unexpected(layout.error());
        }
        cxx::scope_exit release_layout([&]() noexcept { program_cache.releaseLayout(*layout); });
        const auto lookup = program_cache.lookup(retained_shader, **layout, is_raster ? &key : nullptr);
        auto* program = lookup.hit();
        if (!program)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kCommandCancelled});
            }
            const auto compile = [
                &key, &artifact, layout = bindings.layout, is_raster, device = &session.device(), core = &session.nriCore(),
                enabled = session.enabledFeatures(), limits = &session.description(), formats = session.formatSupport(),
                vertex_limits = std::array{session.vertexStrideLimit(), session.vertexOffsetLimit()}
            ]() noexcept -> RenderResult<nri::Pipeline*>
            {
                if (!is_raster) return compileCompute(*device, *core, enabled, *layout, artifact);
                nri::Pipeline* pipeline = nullptr;
                const auto created = createRasterProgram(
                    *device, *core, *limits, enabled, formats, vertex_limits, *layout, artifact, key, pipeline
                );
                if (created) return pipeline;
                if (pipeline) core->DestroyPipeline(pipeline);
                return cxx::unexpected(created.error());
            };
            // Native pipeline creation is thread-safe. Runtime program preparation uses its CPU worker
            // so a stalled driver call cannot prevent native queue operations for ready Jobs.
            using VSchedule = exec::variant_sender<decltype(stdexec::schedule(cpu)),
                decltype(stdexec::schedule(session.nativeScheduler()))>;
            auto scheduled = runtime_current ? VSchedule{stdexec::schedule(cpu)}
                                             : VSchedule{stdexec::schedule(session.nativeScheduler())};
            auto compiled = co_await (
                std::move(scheduled) | stdexec::then(compile) |
                stdexec::upon_error(NativeScheduleFailure<nri::Pipeline*>{}) |
                stdexec::upon_stopped([]() noexcept -> RenderResult<nri::Pipeline*>
                {
                    return cxx::unexpected(RenderError{kCommandCancelled});
                }) |
                stdexec::continues_on(owner_scheduler) | stdexec::upon_error(NativeDeliveryFailure<nri::Pipeline*>{})
            );
            if (runtime_current && !(*runtime_current)())
            {
                if (compiled) session.nriCore().DestroyPipeline(*compiled);
                co_return cxx::unexpected(RenderError{kCommandCancelled});
            }
            if (!compiled)
            {
                co_return cxx::unexpected(compiled.error());
            }
            program = is_raster ? program_cache.publishRaster(std::move(key), lookup, *compiled)
                                : program_cache.publishCompute(lookup, *compiled);
        }
        co_return program;
    }

    exec::task<RenderResult<PreparedRasterStreamProgram>>
    prepareRasterStreamProgram(
        DeviceSession& session, execution::CpuScheduler cpu, const RasterPassTemplate& stage,
        const RasterStreamTarget& target, const rdesc::ShaderProgramArtifact& artifact, const GraphicsState& graphics,
        std::optional<cxx::function_ref<bool() noexcept>> runtime_current
    ) noexcept
    {
        if (artifact.target != session.enabledFeatures().shader_target)
            co_return cxx::unexpected(RenderError{kDeviceUnsupported});
        auto layout = planRasterLayout(session.description(), artifact.interface, stage);
        if (!layout) co_return cxx::unexpected(layout.error());
        std::uint32_t shared_texture_stages = 0;
        std::uint32_t material_stages = 0;
        rdesc::ShaderBlock material_layout;
        for (const auto& field : layout->bindings)
        {
            const bool supported_kind = field.kind == EDescriptorKind::UNIFORM_BUFFER ||
                field.kind == EDescriptorKind::STORAGE_BUFFER || field.kind == EDescriptorKind::SAMPLED_IMAGE ||
                field.kind == EDescriptorKind::SAMPLER;
            if (!supported_kind) co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            if (field.count != 0) continue;

            const bool shared_image = field.kind == EDescriptorKind::SAMPLED_IMAGE && !field.image_arrayed &&
                !field.image_depth;
            const auto slot = std::ranges::find(stage.inputs, field.name, &RasterPassTemplate::Input::name);
            const bool shared_sampler = field.kind == EDescriptorKind::SAMPLER &&
                slot != stage.inputs.end() && !slot->comparison_sampler;
            const bool material_argument = slot != stage.inputs.end() && slot->usage == EGraphUse::STORAGE_READ;
            const bool material_record = field.block.runtime_records && field.block.hasStructuredLayout();
            const bool shared_material = field.kind == EDescriptorKind::STORAGE_BUFFER &&
                field.name == rdesc::kMaterialTableName && material_argument && material_record && !material_stages;
            if (shared_material && target.shared_materials)
            {
                material_stages = field.stages;
                material_layout = field.block;
                continue;
            }
            if (!target.shared_textures || (!shared_image && !shared_sampler))
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            if (shared_image) shared_texture_stages |= field.stages;
        }
        // Shared descriptors are supplied by the device tables. Each draw provides private bindings only.
        std::erase_if(layout->uses, [&](const auto& use) noexcept { return layout->bindings[use.field].count == 0; });

        RasterProgramKey key{graphics, target.colors, target.depth, target.samples};
        if (target.dynamic_depth_bias) key.state.depth_bias_constant = 1;
        auto lease = co_await prepareGraphProgram(std::move(key), artifact, *layout, session, cpu, true, runtime_current);
        if (!lease) co_return cxx::unexpected(lease.error());
        for (auto& field : layout->bindings)
        {
            field.name.clear();
            field.block.fields.clear();
        }
        for (auto& field : layout->private_bindings)
        {
            field.name.clear();
            field.block.fields.clear();
        }
        layout->initial_parameters = {};
        auto mesh_attributes = graphics.vertex_attributes;
        if (artifact.target == rdesc::EShaderTarget::DXIL)
        {
            for (auto& attribute : mesh_attributes)
            {
                const auto input = std::ranges::find(
                    artifact.interface.vertex_inputs, attribute.location, &rdesc::ShaderIoVariable::location
                );
                if (input != artifact.interface.vertex_inputs.end() && input->semantic == "TEXCOORD")
                    attribute.location = input->semantic_index;
            }
        }
        CompiledRasterStream::Program program{
            CompiledPrograms::native(**lease), std::move(*layout), graphics.vertex_bindings,
            std::move(mesh_attributes), shared_texture_stages, material_stages, std::move(material_layout)
        };
        co_return PreparedRasterStreamProgram{std::move(program), *lease};
    }

    exec::task<RenderResult<void>>
    GraphCompiler::prepareOperations(
        const RenderGraphExecutable& plan,
        ShaderBindingPlan& layouts,
        DeviceSession& session,
        NativeGraphPrograms& native,
        const RenderGraph& graph,
        std::span<const GraphPassId> order,
        execution::CpuScheduler cpu
    ) noexcept
    {
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& program_cache = session.programs();
        for (auto& pass : plan.recording_.passes)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kCommandCancelled});
            }
            const auto& source = graph.passes()[order[pass.binding_slot].index];
            const auto& input = source.interface;
            auto& bindings = layouts.passes[pass.binding_slot];
            const rdesc::ShaderProgramArtifact* shader = nullptr;
            std::vector<nri::Format> colors;
            std::optional<PassRenderingInfo> rendering;
            if (const auto* scope = pass.rasterScope())
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
            if (input.program != kGraphInvalidIndex)
            {
                const auto& declaration = graph.programs()[input.program];
                shader = declaration.find(session.enabledFeatures().shader_target, plan.device_addresses_);
                if (!shader)
                {
                    co_return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                RasterProgramKey key;
                const bool is_raster = source.kind == EGraphPassKind::GRAPHICS;
                if (is_raster)
                {
                    key.state = source.graphics.value_or(GraphicsState{});
                    if (rendering->dynamic_depth_bias) key.state.depth_bias_constant = 1;
                    key.colors = colors;
                    key.depth = rendering->depth;
                    key.samples = rendering->samples;
                }
                auto prepared =
                    co_await prepareGraphProgram(std::move(key), *shader, bindings, session, cpu, is_raster);
                if (!prepared) co_return cxx::unexpected(prepared.error());
                auto* program = *prepared;
                native.program_uses_.push_back(program);
                native.slots_[pass.binding_slot].pipeline = CompiledPrograms::native(*program);
            }
            else if (!bindings.private_bindings.empty())
            {
                auto layout = program_cache.acquireLayout(bindings, static_cast<std::uint32_t>(colors.size()));
                if (!layout)
                {
                    co_return cxx::unexpected(layout.error());
                }
                native.layout_uses_.push_back(*layout);
            }
            const auto* raster = std::get_if<RasterWork>(&source.command);
            const auto* description = raster ? std::get_if<RasterPassTemplate>(&raster->program) : nullptr;
            if (description && description->usesStreamProducer())
            {
                auto stream = std::make_unique<CompiledRasterStream>();
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
                    description->shared_textures.has_value(), description->shared_materials.has_value()
                };
                if (description->runtime_stream_programs)
                    stream->runtime = CompiledRasterStream::RuntimeContract{description->inputs, target};
                RasterPassTemplate stage;
                stage.inputs = description->inputs;
                for (const auto& authored : description->stream_programs)
                {
                    const auto* artifact = graph.programs()[authored.program].find(
                        session.enabledFeatures().shader_target, plan.device_addresses_
                    );
                    if (!artifact) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
                    stage.parameters = authored.parameters;
                    auto program = co_await prepareRasterStreamProgram(session, cpu, stage, target, *artifact, authored.graphics);
                    if (!program) co_return cxx::unexpected(program.error());
                    native.program_uses_.push_back(program->lease);
                    stream->programs.push_back(std::move(program->data));
                }
                native.streams_[pass.binding_slot] = std::move(stream);
            }
            native.slots_[pass.binding_slot].descriptors = bindings.recordBindings();
            bindings.layout = nullptr;
        }
        co_return RenderResult<void>{};
    }

    RenderResult<void>
    GraphCompiler::lowerCommands(
        RenderGraphExecutable& plan, const RenderGraph& graph, std::span<const GraphPassId> order,
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
            const auto program = source.interface.program;
            const rdesc::ShaderInfo* shader_info = nullptr;
            if (program != kGraphInvalidIndex)
            {
                const auto* shader = graph.programs()[program].find(target, plan.device_addresses_);
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
            const auto append = [&]<class T>(RenderResult<T> result, ERecordKernel kernel, std::vector<T>& table)
                noexcept -> RenderResult<RecordBlock>
            {
                if (!result) return cxx::unexpected(result.error());
                const auto packet = static_cast<std::uint32_t>(table.size());
                table.push_back(std::move(*result));
                return RecordBlock{kernel, packet};
            };
            auto commands = std::visit(
                [&](const auto& command) noexcept -> RenderResult<RecordBlock>
                {
                    using Command = std::remove_cvref_t<decltype(command)>;
                    if constexpr (std::is_same_v<Command, RasterWork>)
                    {
                        return append(
                            RasterPass::compile(command, context), ERecordKernel::RASTER, plan.recording_.raster
                        );
                    }
                    else if constexpr (std::is_same_v<Command, ComputeWork>)
                    {
                        return append(
                            ComputePass::compile(command, context), ERecordKernel::COMPUTE, plan.recording_.compute
                        );
                    }
                    else
                    {
                        return append(
                            TransferPass::compile(command, source.interface.typed_buffer_writes, context),
                            ERecordKernel::TRANSFER,
                            plan.recording_.transfer
                        );
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
        return {};
    }

    exec::task<RenderResult<PreparedRenderGraph>> GraphCompiler::compile(
        DeviceSession& session, execution::CpuScheduler cpu, const RenderGraph& graph,
        const RenderGraphAnalysis& analysis, const EffectOutputDesc* target, ETransientAllocation allocation,
        bool exercise_async
    ) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        const std::array queues{
            &session.queue(EQueueRole::GRAPHICS).properties(), &session.queue(EQueueRole::COMPUTE).properties(),
            &session.queue(EQueueRole::TRANSFER).properties()
        };
        GraphAssemblyPlan assembly;
        ShaderBindingPlan bindings;
        auto candidate = co_await (
            stdexec::schedule(cpu) |
            stdexec::then(
                [&]() noexcept
                {
                    return planStructure(
                        &session.device(), session.description(), queues, graph, analysis, target, allocation,
                        session.usesDeviceAddresses(), bindings
                    );
                }
            ) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<RenderGraphExecutable> { return cxx::unexpected(RenderError{kCpuStopped}); }
            ) |
            stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<RenderGraphExecutable>{})
        );
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        NativeGraphPrograms native(session, candidate->passes().size());
        auto prepared = co_await prepareOperations(
            *candidate, bindings, session, native, graph, analysis.execution_order, cpu
        );
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        GraphCompileDiagnostics diagnostics;
        auto planned = co_await (
            stdexec::schedule(cpu) |
            stdexec::then(
                [&]() noexcept
                {
                    return finishPlan(
                        *candidate, graph, analysis, queues, session.description(), allocation,
                        diagnostics, {session.graphQueueCosts(), exercise_async}, bindings
                    );
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
        diagnostics.source_passes = analysis.execution_order;
        diagnostics.resource_keys.reserve(graph.resources().size());
        for (const auto& resource : graph.resources())
        {
            diagnostics.resource_keys.push_back(resource.key);
        }
        assembly.source_pass_slots.resize(graph.passes().size());
        for (std::uint32_t slot = 0; slot < analysis.execution_order.size(); ++slot)
        {
            assembly.source_pass_slots[analysis.execution_order[slot].index] = {slot};
        }
        co_return PreparedRenderGraph{
            std::move(*candidate), std::move(native), std::move(assembly), std::move(diagnostics)
        };
    }
} // namespace lux::render::detail
