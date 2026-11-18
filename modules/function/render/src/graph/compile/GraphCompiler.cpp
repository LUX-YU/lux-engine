#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/compile/BarrierPlan.hpp>
#include <lux/engine/render/graph/compile/ConditionalPlan.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/compile/RenderingPlan.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>
#include <ranges>

#include <algorithm>
#include <cmath>

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
            if (scope.state_input && !limits.features.dynamicDepthBias)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            return {};
        }

    } // namespace

    RenderResult<RenderPlan> GraphCompiler::planStructure(
        nri::Device& device, const nri::CoreInterface& core, std::array<const QueueProperties*, 3> queues,
        const RenderGraph& graph, const RenderGraphAnalysis& analysis, const EffectOutputDesc* target,
        ETransientAllocation allocation, std::span<const std::uint32_t> parameter_groups, bool device_addresses
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
        const auto& limits = core.GetDeviceDesc(device);
        const bool is_invalid_address_mode =
            device_addresses && (limits.graphicsAPI != nri::GraphicsAPI::VK || !limits.shaderFeatures.nativeI64);
        if (is_invalid_address_mode)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        RenderPlan plan(device, target ? *target : EffectOutputDesc{});
        plan.device_addresses_ = device_addresses;
        plan.condition_count_ = static_cast<std::uint32_t>(graph.conditions().size());
        std::vector<bool> live(graph.resources().size(), false);
        for (const auto& lifetime : analysis.lifetimes)
        {
            live[lifetime.version.resource.index] = true;
        }
        const auto memory_used = live;
        for (auto input : analysis.parameter_inputs)
        {
            live[input.index] = true;
        }
        for (const auto& data : graph.instanceData() | std::views::reverse)
        {
            if (!data.create)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            for (auto id : data.inputs)
            {
                if (id.index >= live.size())
                {
                    return cxx::unexpected(RenderError{kGraphUnknownResource});
                }
            }
            for (auto id : data.outputs)
            {
                if (id.index >= live.size())
                {
                    return cxx::unexpected(RenderError{kGraphUnknownResource});
                }
            }
            const bool used =
                std::ranges::any_of(data.outputs, [&](auto id) { return live[id.index]; }) ||
                std::ranges::any_of(
                    data.passes, [&](auto id)
                    { return std::ranges::find(analysis.execution_order, id) != analysis.execution_order.end(); }
                );
            if (!used)
            {
                continue;
            }
            for (auto id : data.inputs)
            {
                live[id.index] = true;
            }
            plan.instance_data_.push_back(&data);
        }
        std::ranges::reverse(plan.instance_data_);
        for (std::uint32_t i = 0; i < graph.resources().size(); ++i)
        {
            const auto& resource = graph.resources()[i];
            plan.resources_.push_back({resource.key, resource.description, resource.origin});
            auto& output = plan.resources_.back();
            output.used = live[i];
            output.memory_used = memory_used[i];
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
        auto layouts = planLayouts(limits, graph, analysis, parameter_groups, device_addresses, plan.instanceData());
        if (!layouts)
        {
            return cxx::unexpected(layouts.error());
        }
        plan.descriptor_plan_ = std::make_unique<ShaderBindingPlan>(std::move(*layouts));

        plan.pass_indices_.resize(graph.passes().size(), kGraphInvalidIndex);
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
            pass.source_pass = id.index;
            pass.parameter_offset = parameter_bytes;
            pass.parameter_size = plan.descriptor_plan_->passes[id.index].initial_parameters.size();
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
                pass.rendering =
                    CompiledRasterScope{*scope, source.work.policy().scope == EPassEncodingScope::EXCLUSIVE};
            }
            if (source.graphics && pass.rendering)
            {
                pass.rendering->depth_bias = {source.graphics->depth_bias_constant, source.graphics->depth_bias_slope};
            }
            plan.pass_indices_[id.index] = static_cast<std::uint32_t>(plan.recording_.passes.size());
            plan.recording_.passes.push_back(std::move(pass));
        }
        return plan;
    }

    RenderResult<void> GraphCompiler::finishPlan(
        RenderPlan& plan, const RenderGraph& graph, const RenderGraphAnalysis& analysis,
        std::array<const QueueProperties*, 3> queues, const nri::DeviceDesc& limits, ETransientAllocation allocation
    ) noexcept
    {
        for (const auto& placement : plan.descriptor_plan_->passes)
        {
            for (std::size_t i = 0; i < placement.uses.size(); ++i)
            {
                if (placement.bindings[placement.uses[i].field].count == 0)
                {
                    plan.resources_[placement.uses[i].resource].shared_table = true;
                }
            }
        }
        auto& parameter_plan = *plan.descriptor_plan_;
        parameter_plan.parameter_uses.resize(plan.resources_.size());
        for (std::uint32_t p = 0; p < plan.passes().size(); ++p)
        {
            const auto& pass = plan.passes()[p];
            if (const auto* raster = pass.rasterScope(); raster && raster->state_input)
            {
                parameter_plan.parameter_uses[raster->state_input->resource.index].push_back(p);
            }
            for (const auto& mapping : parameter_plan.passes[pass.source_pass].parameters)
            {
                auto& uses = mapping.resource == kGraphInvalidIndex ? parameter_plan.instance_parameter_uses
                                                                    : parameter_plan.parameter_uses[mapping.resource];
                uses.push_back(p);
                if (mapping.resource == kGraphInvalidIndex)
                {
                    const auto& field = mapping.destination;
                    const auto bytes = field.rows * 4 + (field.columns ? (field.columns - 1) * field.matrix_stride : 0);
                    for (auto& region : parameter_plan.parameter_regions)
                    {
                        if (mapping.source_offset < region.offset + region.layout.size &&
                            region.offset < mapping.source_offset + bytes)
                        {
                            region.passes.push_back(p);
                        }
                    }
                }
            }
        }
        const auto deduplicate = [](auto& uses)
        {
            std::ranges::sort(uses);
            uses.erase(std::unique(uses.begin(), uses.end()), uses.end());
        };
        deduplicate(parameter_plan.instance_parameter_uses);
        for (auto& region : parameter_plan.parameter_regions)
        {
            deduplicate(region.passes);
        }
        for (std::uint32_t resource = 0; resource < parameter_plan.parameter_uses.size(); ++resource)
        {
            auto& uses = parameter_plan.parameter_uses[resource];
            deduplicate(uses);
            if (!uses.empty())
            {
                parameter_plan.parameter_resources.push_back(resource);
            }
        }
        planConditions(graph, plan.recording_);
        selectQueues(graph, queues, plan.recording_);
        // Logical transitions establish the original batch dependency graph before choosing storage.
        auto barriers = planBarriers(
            graph, analysis, *plan.descriptor_plan_, plan.resources_, plan.recording_, plan.output_description_,
            limits.graphicsAPI
        );
        if (!barriers)
        {
            return cxx::unexpected(barriers.error());
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
        const auto use_stages = collectUseStages(graph, *plan.descriptor_plan_, limits.graphicsAPI);
        planSubmissions(
            graph, analysis, plan.resources_, plan.recording_, use_stages, queues, plan.submissions_,
            plan.imported_accesses_, plan.output_
        );
        for (const auto& access : plan.imported_accesses_)
        {
            if (access.resource.index == plan.output_)
            {
                plan.output_first_batch_ = std::min(plan.output_first_batch_, access.batch);
            }
        }
        plan.physical_resources_ =
            planPhysicalResources(graph, plan.resources_, plan.recording_, plan.submissions_, queues, allocation);
        if (std::ranges::find(plan.physical_resources_.reasons, EResourceReuse::REUSED) !=
            plan.physical_resources_.reasons.end())
        {
            // Rebuild only the cold transition templates using the selected physical identities.
            // Their ordering is already proved: no submission dependency or CPU wait is added.
            barriers = planBarriers(
                graph, analysis, *plan.descriptor_plan_, plan.resources_, plan.recording_, plan.output_description_,
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
                for (const auto& [resource, stages] : use_stages[plan.recording_.passes[p].source_pass])
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
                const auto& source = graph.passes()[pass.source_pass];
                const auto* raster = pass.work.getIf<RasterCommands>();
                if (source.side_effect || (raster && std::holds_alternative<IndexedListCommands>(raster->draw)))
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
        return {};
    }

    exec::task<RenderResult<RenderPlan>> GraphCompiler::compile(
        DeviceSession& session, execution::CpuScheduler cpu, const RenderGraph& graph,
        const RenderGraphAnalysis& analysis, const EffectOutputDesc* target, ETransientAllocation allocation,
        std::span<const std::uint32_t> parameter_groups
    ) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        const std::array queues{
            &session.queue(EQueueRole::GRAPHICS).properties(), &session.queue(EQueueRole::COMPUTE).properties(),
            &session.queue(EQueueRole::TRANSFER).properties()
        };
        auto candidate = co_await (
            stdexec::schedule(cpu) |
            stdexec::then(
                [&]() noexcept
                {
                    return planStructure(
                        session.device(), session.nriCore(), queues, graph, analysis, target, allocation,
                        parameter_groups, session.usesDeviceAddresses()
                    );
                }
            ) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<RenderPlan> { return cxx::unexpected(RenderError{kCpuStopped}); }
            ) |
            stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<RenderPlan>{})
        );
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        if (auto prepared = co_await candidate->prepareOperations(session, graph, cpu); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto planned = co_await (
            stdexec::schedule(cpu) |
            stdexec::then([&]() noexcept
                          { return finishPlan(*candidate, graph, analysis, queues, session.description(), allocation); }
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
        co_return std::move(*candidate);
    }
} // namespace lux::render::detail
