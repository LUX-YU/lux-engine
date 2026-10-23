#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/BarrierPlan.hpp>
#include <lux/engine/render/graph/ConditionalPlan.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/graph/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/graph/RenderingPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/SubmissionPlan.hpp>

#include <algorithm>
#include <cmath>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<nri::PipelineLayout*> createLayout(
            nri::Device& device,
            const nri::CoreInterface& core,
            const PassLayout& program,
            std::uint32_t image_capacity,
            std::uint32_t sampler_capacity
        ) noexcept
        {
            const auto shared = DescriptorTable::layoutRanges(image_capacity, sampler_capacity);
            const auto local = descriptorRanges(program.private_bindings, 0, 0);
            std::vector<nri::DescriptorSetDesc> sets;
            if (program.shared)
            {
                sets.push_back(
                    {0,
                     shared.data(),
                     static_cast<std::uint32_t>(shared.size()),
                     nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET}
                );
            }
            if (!local.empty())
            {
                sets.push_back({program.private_set, local.data(), static_cast<std::uint32_t>(local.size())});
            }
            const nri::RootConstantDesc parameters{0, program.parameter_bytes, program.parameter_stages};
            nri::PipelineLayoutDesc description{};
            description.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
            description.shaderStages =
                nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;
            description.descriptorSets = sets.data();
            description.descriptorSetNum = static_cast<std::uint32_t>(sets.size());
            description.rootConstants = program.parameter_bytes ? &parameters : nullptr;
            description.rootConstantNum = unsigned(program.parameter_bytes != 0);
            nri::PipelineLayout* layout = nullptr;
            const auto created = core.CreatePipelineLayout(device, description, layout);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            return layout;
        }

        bool sameLayout(const PassLayout& a, const PassLayout& b) noexcept
        {
            const auto same_binding = [](const ShaderBinding& left, const ShaderBinding& right)
            {
                return left.set == right.set && left.binding == right.binding && left.kind == right.kind &&
                       left.count == right.count && left.stages == right.stages;
            };
            return a.shared == b.shared && std::ranges::equal(a.private_bindings, b.private_bindings, same_binding) &&
                   a.parameter_bytes == b.parameter_bytes && a.parameter_stages == b.parameter_stages;
        }

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
                                const bool is_same_extent = aw == bw && ah == bh && a.extent == b.extent;
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

        RenderResult<void> validateDraw(
            const RasterWork& work,
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
                                                  draw.selection_parameter.empty() || draw.selection_count == 0;
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
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        std::array<const QueueProperties*, 3> queues,
        std::uint32_t image_capacity,
        std::uint32_t sampler_capacity,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        const TargetDesc* target,
        ETransientAllocation allocation
    ) noexcept
    {
        if (!queues[0] || !queues[1] || !queues[2] || queues[0]->type != nri::QueueType::GRAPHICS)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        if (target)
        {
            if (auto valid = target->validate(); !valid)
            {
                return cxx::unexpected(valid.error());
            }
        }
        const auto& limits = core.GetDeviceDesc(device);
        RenderPlan plan(device, core, target ? *target : TargetDesc{});
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
                (image && (image->extent.numerator != image->extent.denominator || image->extent.alignment != 1)) ||
                (!relative && (requirement.width != target->width || requirement.height != target->height));
            if (incompatible)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            plan.output_ = index;
            plan.resizable_output_ = relative;
            resource.target_sized = relative;
        }
        auto layouts = planLayouts(limits, graph, analysis);
        if (!layouts)
        {
            return cxx::unexpected(layouts.error());
        }
        plan.descriptor_plan_ = std::make_unique<PipelineLayoutPlan>(std::move(*layouts));

        struct PipelineIdentity final
        {
            rdesc::SpirvArtifact shader;
            nri::PipelineLayout* layout;
            GraphicsState state;
            std::vector<nri::Format> colors;
            nri::Format depth = nri::Format::UNKNOWN;
            std::uint32_t samples = 1;
            bool operator==(const PipelineIdentity&) const noexcept = default;
        };

        std::vector<PipelineIdentity> identities;
        plan.pass_indices_.resize(graph.passes().size(), kGraphInvalidIndex);
        std::size_t parameter_bytes = 0;
        std::uint32_t draw_list_count = 0;
        for (const auto id : analysis.execution_order)
        {
            const auto& source = graph.passes()[id.index];
            if (source.uses.empty())
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            CompiledPass pass;
            pass.source_pass = id.index;
            pass.parameter_offset = parameter_bytes;
            pass.parameter_size = plan.descriptor_plan_->passes[id.index].initial_parameters.size();
            parameter_bytes += pass.parameter_size;
            if (source.condition && source.condition->source == EConditionSource::HISTORY_VALID &&
                plan.resources_[source.condition->input].history == kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            const PassInterface* input = nullptr;
            PipelineIdentity identity{};
            if (const auto* raster = std::get_if<RasterWork>(&source.work))
            {
                input = &raster->interface;
                if (raster->colors.empty() && !raster->depth)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const auto& reference = raster->colors.empty() ? *raster->depth : raster->colors.front();
                const auto& reference_desc =
                    std::get<GraphImageDesc>(graph.resources()[reference.access.version.resource.index].description);
                const auto& reference_range = std::get<GraphImageRange>(reference.access.range);
                identity.samples = reference_desc.samples;
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
                    if (mismatch || attachment.load > EAttachmentLoad::LOAD ||
                        attachment.store > EAttachmentStore::STORE || (depth && attachment.resolve))
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
                if (!std::ranges::all_of(
                        raster->colors,
                        [&](const auto& color) { return validateAttachment(color, false); }
                    ) ||
                    (raster->depth && !validateAttachment(*raster->depth, true)))
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                for (const auto& color : raster->colors)
                {
                    const auto& description =
                        std::get<GraphImageDesc>(plan.resources_[color.access.version.resource.index].description);
                    identity.colors.push_back(imageFormat(description.format).native);
                }
                if (raster->depth)
                {
                    const auto& description = std::get<GraphImageDesc>(
                        plan.resources_[raster->depth->access.version.resource.index].description
                    );
                    identity.depth = imageFormat(description.format).native;
                }
                identity.state = raster->graphics;
                if (raster->state_input)
                {
                    if (!limits.features.dynamicDepthBias)
                    {
                        return cxx::unexpected(RenderError{kDeviceUnsupported});
                    }
                    // Keep bias enabled in the pipeline; the prepared value may be exactly zero.
                    identity.state.depth_bias_constant = 1;
                }
                if (auto valid = validateDraw(*raster, graph, limits); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
                auto& recorded = std::get<RasterCommands>(pass.work);
                recorded.colors = raster->colors;
                recorded.depth = raster->depth;
                if (const auto* list = std::get_if<DrawIndexedList>(&raster->draw))
                {
                    const auto reflected = shaderInterface(graph.programs()[input->program].interface);
                    if (!reflected)
                    {
                        return cxx::unexpected(reflected.error());
                    }
                    const auto& fields = reflected->parameters.fields;
                    const auto field = std::ranges::find(fields, list->selection_parameter, &rdesc::ShaderMember::path);
                    const bool invalid_selection = field == fields.end() || field->scalar != rdesc::EDataScalar::UINT ||
                                                   field->rows != 1 || field->columns != 1 || field->array_stride != 0;
                    if (invalid_selection)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                    recorded.draw = IndexedListCommands{
                        list->index_resource,
                        list->index_offset,
                        list->type,
                        field->offset,
                        list->selection_count,
                        draw_list_count++
                    };
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
                        raster->draw
                    );
                }
                recorded.viewport = raster->viewport;
                recorded.state_input = raster->state_input;
                recorded.depth_bias = {raster->graphics.depth_bias_constant, raster->graphics.depth_bias_slope};
                if (raster->viewport && !raster->viewport->validate())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                for (const auto& source : raster->vertex_buffers)
                {
                    const auto binding =
                        std::ranges::find(raster->graphics.vertex_bindings, source.binding, &VertexBinding::binding);
                    recorded.vertices.push_back(
                        {source.resource.resource, source.binding, source.offset, binding->stride}
                    );
                }
            }
            else if (const auto* compute = std::get_if<ComputeWork>(&source.work))
            {
                input = &compute->interface;
                pass.work = ComputeCommands{compute->dispatch};
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
            if (!input)
            {
                pass.work = std::get<TransferWork>(source.work);
                if (auto valid = validateTransfer(std::get<TransferWork>(source.work), graph); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
            }
            auto& placement = plan.descriptor_plan_->passes[id.index];
            if (input || !placement.private_bindings.empty())
            {
                if (placement.shared && (!image_capacity || !sampler_capacity))
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                for (const auto previous : analysis.execution_order)
                {
                    if (previous == id)
                    {
                        break;
                    }
                    const auto& other = plan.descriptor_plan_->passes[previous.index];
                    if (other.layout && sameLayout(placement, other))
                    {
                        placement.layout = other.layout;
                        break;
                    }
                }
                if (!placement.layout)
                {
                    auto layout = createLayout(device, core, placement, image_capacity, sampler_capacity);
                    if (!layout)
                    {
                        return cxx::unexpected(layout.error());
                    }
                    placement.layout = *layout;
                    plan.layouts_.push_back(*layout);
                }
            }
            if (input)
            {
                auto shader = relocateShader(graph.programs()[input->program], placement.bindings);
                if (!shader)
                {
                    return cxx::unexpected(shader.error());
                }
                identity.shader = std::move(*shader);
                identity.layout = placement.layout;
                const auto found = std::ranges::find(identities, identity);
                if (found != identities.end())
                {
                    pass.pipeline = plan.pipelines_[found - identities.begin()];
                }
                else
                {
                    auto pipeline = std::holds_alternative<ComputeWork>(source.work)
                                        ? compileCompute(device, core, enabled, *identity.layout, identity.shader)
                                        : compileGraphics(
                                              device,
                                              core,
                                              enabled,
                                              formats,
                                              vertex_limits,
                                              *identity.layout,
                                              identity.shader,
                                              identity.state,
                                              identity.colors,
                                              identity.depth,
                                              identity.samples
                                          );
                    if (!pipeline)
                    {
                        auto error = pipeline.error();
                        error.args[2] = std::uint64_t(id.index) + 1;
                        return cxx::unexpected(error);
                    }
                    pass.pipeline = *pipeline;
                    plan.pipelines_.push_back(*pipeline);
                    identities.push_back(std::move(identity));
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
        planConditions(graph, plan.recording_);
        selectQueues(graph, queues, plan.recording_);
        // Logical transitions establish the original batch dependency graph before choosing storage.
        auto barriers =
            planBarriers(graph, analysis, *plan.descriptor_plan_, plan.resources_, plan.recording_, plan.target_);
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
        planSubmissions(
            graph,
            analysis,
            plan.resources_,
            plan.recording_,
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
                plan.target_,
                &plan.physical_resources_
            );
            if (!barriers)
            {
                return cxx::unexpected(barriers.error());
            }
        }
        auto rendering = planRendering(analysis, plan.resources_, plan.recording_);
        if (!rendering)
        {
            return cxx::unexpected(rendering.error());
        }
        return plan;
    }
} // namespace lux::render::detail
