#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/compile/BarrierPlan.hpp>
#include <lux/engine/render/graph/compile/ConditionalPlan.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/compile/RenderingPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>

#include <algorithm>
#include <cmath>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<void> validateTransfer(const TransferWork& work, const RenderGraph& graph) noexcept
        {
            for (const auto& operation : work.operations)
            {
                const bool valid = std::visit(
                    [&](const auto& copy)
                    {
                        using Operation = std::remove_cvref_t<decltype(copy)>;
                        const auto& destination =
                            graph.resources()[copy.destination.version.resource.index].description;
                        const auto* to_buffer = std::get_if<GraphBufferRange>(&copy.destination.range);
                        const auto* to_image = std::get_if<GraphImageRange>(&copy.destination.range);
                        if constexpr (std::is_same_v<Operation, FillBuffer>)
                        {
                            return to_buffer && to_buffer->offset % 4 == 0 && to_buffer->size % 4 == 0;
                        }
                        else if constexpr (std::is_same_v<Operation, ClearImage>)
                        {
                            const bool is_valid_range = to_image && to_image->mip_count == 1;
                            const bool is_valid_clear =
                                std::isfinite(copy.depth) &&
                                std::ranges::all_of(copy.color, [](float value) { return std::isfinite(value); });
                            return is_valid_range && is_valid_clear;
                        }
                        else
                        {
                            const auto& source = graph.resources()[copy.source.version.resource.index].description;
                            const auto* from_buffer = std::get_if<GraphBufferRange>(&copy.source.range);
                            const auto* from_image = std::get_if<GraphImageRange>(&copy.source.range);
                            if constexpr (std::is_same_v<Operation, CopyBuffer>)
                            {
                                return from_buffer && to_buffer && from_buffer->size == to_buffer->size;
                            }
                            else if constexpr (std::is_same_v<Operation, CopyImage>)
                            {
                                if (!from_image || !to_image)
                                {
                                    return false;
                                }
                                const auto& a = std::get<GraphImageDesc>(source);
                                const auto& b = std::get<GraphImageDesc>(destination);
                                const auto aw = copy.source_region ? copy.source_region->width
                                                                   : std::max(1u, a.width >> from_image->first_mip);
                                const auto ah = copy.source_region ? copy.source_region->height
                                                                   : std::max(1u, a.height >> from_image->first_mip);
                                const auto bw = copy.destination_region ? copy.destination_region->width
                                                                        : std::max(1u, b.width >> to_image->first_mip);
                                const auto bh = copy.destination_region ? copy.destination_region->height
                                                                        : std::max(1u, b.height >> to_image->first_mip);
                                const bool is_same_format = a.format == b.format && a.samples == b.samples;
                                const bool comparable_mips = a.extent.source == EImageExtentSource::FIXED ||
                                    from_image->first_mip == to_image->first_mip;
                                const bool is_same_extent =
                                    comparable_mips && aw == bw && ah == bh && a.extent == b.extent;
                                const bool is_same_subresources = from_image->aspects == to_image->aspects &&
                                                                  from_image->mip_count == to_image->mip_count &&
                                                                  from_image->layer_count == to_image->layer_count;
                                return is_same_format && is_same_extent && is_same_subresources;
                            }
                            else
                            {
                                const bool kinds = (from_image && to_buffer) || (from_buffer && to_image);
                                if (!kinds)
                                {
                                    return false;
                                }
                                const auto& image = std::get<GraphImageDesc>(from_image ? source : destination);
                                const auto& range = from_image ? *from_image : *to_image;
                                return image.samples == 1 && range.mip_count == 1 && range.layer_count == 1;
                            }
                        }
                    },
                    operation
                );
                if (!valid)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            return {};
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
                    std::get<GraphImageDesc>(graph.resources()[attachment.access.version.resource.index].description
                    );
                const auto& range = std::get<GraphImageRange>(attachment.access.range);
                const bool mismatch =
                    description.samples != reference_desc.samples || range.mip_count != 1 ||
                    range.layer_count != reference_range.layer_count ||
                    (description.width >> range.first_mip) != (reference_desc.width >> reference_range.first_mip) ||
                    (description.height >> range.first_mip) !=
                        (reference_desc.height >> reference_range.first_mip) ||
                    description.extent != reference_desc.extent;
                const bool is_invalid_mode = attachment.load > EAttachmentLoad::LOAD ||
                    attachment.store > EAttachmentStore::STORE;
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
                        description.samples == 1 || resolved.samples != 1 ||
                        description.format != resolved.format || range.mip_count != resolved_range.mip_count ||
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
            const bool has_invalid_color = !std::ranges::all_of(raster->colors,
                [&](const auto& color) { return validateAttachment(color, false); });
            const bool has_invalid_depth = raster->depth && !validateAttachment(*raster->depth, true);
            const bool is_invalid_scope = has_invalid_color || has_invalid_depth;
            if (is_invalid_scope)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if (scope.viewport && !scope.viewport->validate())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            if (scope.state_input && !limits.features.dynamicDepthBias)
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            return {};
        }

        RenderResult<void> validateDraw(
            const RasterWork::FixedProgram& work,
            const RenderGraph& graph,
            const nri::DeviceDesc& limits
        ) noexcept
        {
            const auto bufferSize = [&](ResourceVersion version) noexcept
            {
                const auto* desc = std::get_if<GraphBufferDesc>(&graph.resources()[version.resource.index].description);
                return desc ? desc->size : 0;
            };
            if (work.vertex_buffers.size() != work.graphics.vertex_bindings.size())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            for (const auto& vertex : work.vertex_buffers)
            {
                const bool duplicate =
                    std::ranges::count(work.vertex_buffers, vertex.binding, &ProgramVertexBuffer::binding) != 1;
                if (duplicate)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const auto binding =
                    std::ranges::find(work.graphics.vertex_bindings, vertex.binding, &VertexBinding::binding);
                if (binding == work.graphics.vertex_bindings.end() || !binding->stride ||
                    vertex.offset >= bufferSize(vertex.resource))
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            return std::visit(
                [&](const auto& draw) noexcept -> RenderResult<void>
                {
                    using Draw = std::remove_cvref_t<decltype(draw)>;
                    if constexpr (!std::is_same_v<Draw, DrawVertices>)
                    {
                        const auto bytes = draw.type == EIndexType::UINT16 ? 2u : 4u;
                        const auto offset = [&]() noexcept
                        {
                            if constexpr (std::is_same_v<Draw, DrawIndexed>)
                            {
                                return draw.offset;
                            }
                            else
                            {
                                return draw.index_offset;
                            }
                        }();
                        const auto size = bufferSize(draw.index_resource);
                        const bool invalid_index =
                            draw.type > EIndexType::UINT32 || offset % bytes || offset >= size || size - offset < bytes;
                        if (invalid_index)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        if constexpr (std::is_same_v<Draw, DrawIndexed>)
                        {
                            if (std::uint64_t(draw.first) + draw.count > (size - offset) / bytes)
                            {
                                return cxx::unexpected(RenderError{kDeviceArgument});
                            }
                        }
                    }
                    if constexpr (std::is_same_v<Draw, DrawIndexedIndirect>)
                    {
                        const auto size = bufferSize(draw.commands_resource);
                        const bool invalid_range = !draw.count || draw.commands_offset > size ||
                                                   draw.commands_offset % 4 ||
                                                   draw.stride < sizeof(nri::DrawIndexedDesc) || draw.stride % 4 ||
                                                   (draw.commands_offset <= size &&
                                                    (size - draw.commands_offset < sizeof(nri::DrawIndexedDesc) ||
                                                     std::uint64_t(draw.count - 1) * draw.stride >
                                                         size - draw.commands_offset - sizeof(nri::DrawIndexedDesc)));
                        if (invalid_range || draw.count > limits.other.drawIndirectMaxNum)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        if (draw.count_resource)
                        {
                            const auto count_bytes = bufferSize(*draw.count_resource);
                            if (!limits.features.drawIndirectCount || draw.count_offset % 4 ||
                                draw.count_offset > count_bytes || count_bytes - draw.count_offset < 4)
                            {
                                return cxx::unexpected(RenderError{kDeviceUnsupported});
                            }
                        }
                    }
                    else if constexpr (std::is_same_v<Draw, DrawIndexedList>)
                    {
                        const bool invalid_list = work.vertex_buffers.size() != 1 || draw.index_offset != 0 ||
                                                  work.vertex_buffers.front().offset != 0 ||
                                                  draw.selection_parameter.empty();
                        if (invalid_list)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                    }
                    else
                    {
                        if (!draw.count || !draw.instances)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        for (const auto& vertex : work.vertex_buffers)
                        {
                            const auto binding = std::ranges::find(
                                work.graphics.vertex_bindings,
                                vertex.binding,
                                &VertexBinding::binding
                            );
                            std::uint64_t end = 0;
                            if (binding->rate == EVertexRate::INSTANCE)
                            {
                                end = std::uint64_t(draw.first_instance) + draw.instances;
                            }
                            else if constexpr (std::is_same_v<Draw, DrawVertices>)
                            {
                                end = std::uint64_t(draw.first) + draw.count;
                            }
                            if (end > (bufferSize(vertex.resource) - vertex.offset) / binding->stride)
                            {
                                return cxx::unexpected(RenderError{kDeviceArgument});
                            }
                        }
                    }
                    return {};
                },
                work.draw
            );
        }
    } // namespace

    RenderResult<RenderPlan> GraphCompiler::compile(
        nri::Device& device,
        const nri::CoreInterface& core,
        std::array<const QueueProperties*, 3> queues,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        const EffectOutputDesc* target,
        ETransientAllocation allocation,
        std::span<const std::uint32_t> parameter_groups
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
        RenderPlan plan(device, target ? *target : EffectOutputDesc{});
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
            resource.sampled_target_input = resource.memory_used &&
                resource.origin == EGraphOrigin::IMPORTED &&
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
        auto layouts = planLayouts(limits, graph, analysis, parameter_groups);
        if (!layouts)
        {
            return cxx::unexpected(layouts.error());
        }
        plan.descriptor_plan_ = std::make_unique<ShaderBindingPlan>(std::move(*layouts));

        plan.pass_indices_.resize(graph.passes().size(), kGraphInvalidIndex);
        std::size_t parameter_bytes = 0;
        std::uint32_t draw_list_count = 0;
        std::uint32_t raster_list_count = 0;
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
                    return cxx::unexpected(valid.error());
                pass.rendering = CompiledRasterScope{
                    *scope, source.work.policy().scope == EPassEncodingScope::EXCLUSIVE
                };
            }
            const PassInterface* input = nullptr;
            if (const auto* raster = source.work.getIf<RasterWork>())
            {
                const auto* fixed = std::get_if<RasterWork::FixedProgram>(&raster->program);
                input = fixed ? &fixed->interface : nullptr;
                const bool owns_rendering = !fixed || std::holds_alternative<DrawIndexedList>(fixed->draw);
                pass.rendering->exclusive = owns_rendering;
                auto encoding = PassEncoding::create(RasterCommands{}, {
                    owns_rendering ? EPassEncodingScope::EXCLUSIVE : EPassEncodingScope::MANAGED, owns_rendering
                });
                if (!encoding) return cxx::unexpected(encoding.error());
                pass.work = std::move(*encoding);
                auto& recorded = pass.work.get<RasterCommands>();
                if (fixed)
                {
                    if (auto valid = validateDraw(*fixed, graph, limits); !valid)
                    {
                        return cxx::unexpected(valid.error());
                    }
                    if (const auto* list = std::get_if<DrawIndexedList>(&fixed->draw))
                    {
                        const auto reflected = shaderInterface(graph.programs()[input->program].find(
                            limits.graphicsAPI == nri::GraphicsAPI::D3D12
                                ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV
                        )->interface);
                        if (!reflected)
                        {
                            return cxx::unexpected(reflected.error());
                        }
                        const auto& fields = reflected->parameters.fields;
                        const auto field =
                            std::ranges::find(fields, list->selection_parameter, &rdesc::ShaderMember::path);
                        const bool missing_selection = field == fields.end();
                        const bool wrong_selection =
                            !missing_selection && (field->scalar != rdesc::EDataScalar::UINT || field->rows != 1 ||
                                                   field->columns != 1 || field->array_stride != 0);
                        if (missing_selection || wrong_selection)
                        {
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                        }
                        recorded.draw = IndexedListCommands{
                            list->index_resource,
                            list->index_offset,
                            list->type,
                            field->offset,
                            draw_list_count++
                        };
                        auto& compiled_list = std::get<IndexedListCommands>(recorded.draw);
                        compiled_list.partial_parameters = limits.features.rootConstantsOffset;
                    }
                    else
                    {
                        std::visit(
                            [&](const auto& draw) noexcept
                            {
                                if constexpr (!std::same_as<std::decay_t<decltype(draw)>, DrawIndexedList>)
                                {
                                    recorded.draw = draw;
                                }
                            },
                            fixed->draw
                        );
                    }
                    pass.rendering->depth_bias = {fixed->graphics.depth_bias_constant, fixed->graphics.depth_bias_slope};
                    for (const auto& source : fixed->vertex_buffers)
                    {
                        const auto binding =
                            std::ranges::find(fixed->graphics.vertex_bindings, source.binding, &VertexBinding::binding);
                        recorded.vertices.push_back(
                            {source.resource.resource, source.binding, source.offset, binding->stride}
                        );
                    }
                }
                else
                {
                    recorded.draw =
                        RasterListCommands{raster_list_count++, std::get<RasterPassTemplate>(raster->program)};
                }
            }
            else if (const auto* compute = source.work.getIf<ComputeWork>())
            {
                input = &compute->interface;
                auto encoding = PassEncoding::create(ComputeCommands{compute->dispatch});
                if (!encoding) return cxx::unexpected(encoding.error());
                pass.work = std::move(*encoding);
                if (const auto* direct = std::get_if<DispatchArguments>(&compute->dispatch))
                {
                    for (unsigned axis = 0; axis < 3; ++axis)
                    {
                        if (!direct->groups[axis] ||
                            direct->groups[axis] > limits.shaderStage.compute.dispatchMaxDim[axis])
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                    }
                }
                else if (const auto* dispatch = std::get_if<ImageDispatchArguments>(&compute->dispatch))
                {
                    const auto resource = dispatch->resource.index;
                    const auto* image = resource < plan.resources_.size()
                                            ? std::get_if<GraphImageDesc>(&plan.resources_[resource].description)
                                            : nullptr;
                    const bool invalid = !image || dispatch->mip >= image->mips || !dispatch->group_size[0] ||
                                         !dispatch->group_size[1] || !plan.resources_[resource].memory_used;
                    if (invalid)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                }
                else
                {
                    const auto& indirect = std::get<IndirectDispatchArguments>(compute->dispatch);
                    const auto& description =
                        std::get<GraphBufferDesc>(graph.resources()[indirect.buffer.resource.index].description);
                    const bool invalid = indirect.offset % 4 || indirect.offset > description.size ||
                                         description.size - indirect.offset < 12;
                    if (invalid)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                }
            }
            if (source.work.getIf<TransferWork>() != nullptr)
            {
                auto encoding = PassEncoding::create(
                    TransferCommands{source.work.get<TransferWork>().operations}, {EPassEncodingScope::MANAGED, true}
                );
                if (!encoding) return cxx::unexpected(encoding.error());
                pass.work = std::move(*encoding);
                if (auto valid = validateTransfer(source.work.get<TransferWork>(), graph); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
            }
            plan.pass_indices_[id.index] = static_cast<std::uint32_t>(plan.recording_.passes.size());
            plan.recording_.passes.push_back(std::move(pass));
        }
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
                auto& uses = mapping.resource == kGraphInvalidIndex
                    ? parameter_plan.instance_parameter_uses : parameter_plan.parameter_uses[mapping.resource];
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
        for (auto& region : parameter_plan.parameter_regions) deduplicate(region.passes);
        for (std::uint32_t resource = 0; resource < parameter_plan.parameter_uses.size(); ++resource)
        {
            auto& uses = parameter_plan.parameter_uses[resource];
            deduplicate(uses);
            if (!uses.empty()) parameter_plan.parameter_resources.push_back(resource);
        }
        planConditions(graph, plan.recording_);
        selectQueues(graph, queues, plan.recording_);
        // Logical transitions establish the original batch dependency graph before choosing storage.
        auto barriers =
            planBarriers(
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
            graph,
            analysis,
            plan.resources_,
            plan.recording_,
            use_stages,
            queues,
            plan.submissions_,
            plan.imported_accesses_,
            plan.output_
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
                graph,
                analysis,
                *plan.descriptor_plan_,
                plan.resources_,
                plan.recording_,
                plan.output_description_,
                limits.graphicsAPI,
                &plan.physical_resources_
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
                    if (resource != plan.output_) continue;
                    const bool includes_all = found_output_use &&
                        (plan.output_wait_stages_ == nri::StageBits::ALL || stages == nri::StageBits::ALL);
                    plan.output_wait_stages_ = !found_output_use ? stages
                        : includes_all ? nri::StageBits::ALL : plan.output_wait_stages_ | stages;
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
                if (plan.output_description_.kind == ETargetKind::SURFACE && std::ranges::any_of(source.uses,
                    [&](const auto& use) noexcept { return use.version.resource.index == plan.output_; }))
                {
                    batch.output_begin = std::min(batch.output_begin, p);
                }
            }
            if (plan.output_description_.kind == ETargetKind::SURFACE && index + 1 == plan.submissions_.size() &&
                std::ranges::any_of(plan.recording_.after.images,
                    [&](const auto& barrier) noexcept { return barrier.resource == plan.output_; }))
            {
                batch.output_begin = std::min(batch.output_begin, batch.first_pass + batch.pass_count);
            }
            if (batch.output_begin != kGraphInvalidIndex)
            {
                auto begin = batch.output_begin;
                do
                {
                    batch.output_begin = begin;
                    if (begin == batch.first_pass + batch.pass_count) break;
                    const auto& pass = plan.recording_.passes[begin];
                    if (pass.rendering_group != kGraphInvalidIndex)
                        begin = std::min(begin, plan.recording_.groups[pass.rendering_group].first_pass);
                    if (pass.condition_region != kGraphInvalidIndex)
                        begin = std::min(begin, plan.recording_.conditions[pass.condition_region].first_pass);
                } while (begin != batch.output_begin);
            }
        }
        compactImageTransitions(plan.resources_, plan.recording_);
        return plan;
    }
} // namespace lux::render::detail
