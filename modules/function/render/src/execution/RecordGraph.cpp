#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RecordGraph.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>
#include <numeric>

namespace lux::render::detail
{
    void recordGraphPreparation(ExecutionContext& context, nri::CommandBuffer& command, std::uint32_t slot) noexcept
    {
        for (auto index : context.prepared_data_)
        {
            auto& data = context.context_data_[index];
            data.record(data.value.get(), command, slot);
        }
        context.prepared_data_.clear();
    }

    void recordGraph(ExecutionContext& context, nri::CommandBuffer& command, std::uint32_t slot) noexcept
    {
        const auto& nri_core = context.session_->nriCore();
        const auto& code = *context.code_;
        const auto& frame = context.frames_[slot];
        const auto& bindings = frame.bindings;
        nri::BindPoint active_layout = nri::BindPoint::INHERIT;
        for (std::size_t p = 0; p < frame.passes.size(); ++p)
        {
            const auto& pass = frame.passes[p];
            const auto& recipe = *pass.recipe;
            auto& parameters = context.pass_parameters_[p];
            for (const auto& copy : context.parameter_copies_[p])
            {
                const auto* source = copy.resource == kGraphInvalidIndex ? context.context_values_.data()
                                                                         : bindings[copy.resource].parameters.data();
                std::memcpy(parameters.data() + copy.destination_offset, source + copy.source_offset, copy.bytes);
            }
            if (!pass.images.empty() || !pass.buffers.empty())
            {
                const nri::BarrierDesc barriers{
                    nullptr,
                    0,
                    pass.buffers.data(),
                    static_cast<std::uint32_t>(pass.buffers.size()),
                    pass.images.data(),
                    static_cast<std::uint32_t>(pass.images.size())
                };
                nri_core.CmdBarrier(command, barriers);
            }
            const auto& destination = bindings[recipe.destination];
            if (recipe.recipe == ERecipe::DRAW || recipe.recipe == ERecipe::DISPATCH)
            {
                const auto point = recipe.recipe == ERecipe::DRAW ? nri::BindPoint::GRAPHICS : nri::BindPoint::COMPUTE;
                if (point != active_layout)
                {
                    nri_core.CmdSetPipelineLayout(command, point, *code.layout());
                    active_layout = point;
                }
                if (recipe.recipe == ERecipe::DISPATCH || recipe.bind_pipeline)
                {
                    nri_core.CmdSetPipeline(command, *recipe.pipeline);
                }
                for (std::uint32_t set = 0; set < pass.descriptors.size(); ++set)
                {
                    nri_core.CmdSetDescriptorSet(command, {set, pass.descriptors[set], point});
                }
                if (!parameters.empty())
                {
                    nri_core.CmdSetRootConstants(
                        command,
                        {0, parameters.data(), static_cast<std::uint32_t>(parameters.size()), 0, point}
                    );
                }
            }
            switch (recipe.recipe)
            {
            case ERecipe::DRAW:
            {
                if (recipe.begin_rendering)
                {
                    nri::AttachmentDesc color{};
                    if (recipe.color != kGraphInvalidIndex)
                    {
                        color.descriptor = bindings[recipe.color].view;
                    }
                    color.loadOp = recipe.load;
                    color.clearValue.color.f =
                        {recipe.clear_color[0], recipe.clear_color[1], recipe.clear_color[2], recipe.clear_color[3]};
                    nri::RenderingDesc rendering{};
                    rendering.colors = &color;
                    rendering.colorNum = recipe.color != kGraphInvalidIndex;
                    if (recipe.depth != kGraphInvalidIndex)
                    {
                        rendering.depth.descriptor = bindings[recipe.depth].view;
                        rendering.depth.loadOp = recipe.depth_load;
                        rendering.depth.clearValue.depthStencil.depth = recipe.clear_depth;
                    }
                    nri_core.CmdBeginRendering(command, rendering);
                    const auto& image = nri_core.GetTextureDesc(*destination.image);
                    const auto& area = context.viewport_;
                    // Preserve the existing Vulkan clip/viewport convention. NRI's default flips Y.
                    const nri::Viewport viewport{
                        area.x * image.width,
                        area.y * image.height,
                        area.width * image.width,
                        area.height * image.height,
                        area.min_depth,
                        area.max_depth,
                        true
                    };
                    const nri::Rect scissor{0, 0, image.width, image.height};
                    nri_core.CmdSetViewports(command, &viewport, 1);
                    nri_core.CmdSetScissors(command, &scissor, 1);
                }
                for (const auto& source : recipe.vertices)
                {
                    const auto& binding = bindings[source.resource];
                    const nri::VertexBufferDesc buffer{
                        binding.buffer,
                        binding.buffer_offset + source.offset,
                        source.stride
                    };
                    nri_core.CmdSetVertexBuffers(command, source.binding, &buffer, 1);
                }
                if (recipe.draw != EDrawRecipe::VERTICES)
                {
                    const auto& index = bindings[recipe.index_resource];
                    nri_core.CmdSetIndexBuffer(
                        command,
                        *index.buffer,
                        index.buffer_offset + recipe.index_offset,
                        recipe.index_type
                    );
                }
                if (recipe.draw == EDrawRecipe::VERTICES)
                {
                    nri_core.CmdDraw(command, {recipe.count, recipe.instances, recipe.first, recipe.first_instance});
                }
                else if (recipe.draw == EDrawRecipe::INDEXED)
                {
                    nri_core.CmdDrawIndexed(
                        command,
                        {recipe.count, recipe.instances, recipe.first, recipe.vertex_offset, recipe.first_instance}
                    );
                }
                else
                {
                    const auto& indirect = bindings[recipe.indirect_resource];
                    nri_core.CmdDrawIndexedIndirect(
                        command,
                        *indirect.buffer,
                        indirect.buffer_offset + recipe.indirect_offset,
                        recipe.count,
                        recipe.indirect_stride,
                        nullptr,
                        0
                    );
                }
                if (recipe.end_rendering)
                {
                    nri_core.CmdEndRendering(command);
                }
                break;
            }
            case ERecipe::DISPATCH:
                nri_core.CmdDispatch(command, {recipe.dispatch[0], recipe.dispatch[1], recipe.dispatch[2]});
                break;
            case ERecipe::CLEAR_IMAGE:
            {
                nri::RenderingDesc rendering{};
                nri::AttachmentDesc attachment{};
                attachment.descriptor = destination.view;
                attachment.loadOp = nri::LoadOp::CLEAR;
                const auto format =
                    imageFormat(std::get<GraphImageDesc>(code.resources()[recipe.destination].description).format);
                if (format.aspects == nri::PlaneBits::DEPTH)
                {
                    attachment.clearValue.depthStencil.depth = recipe.clear_depth;
                    rendering.depth = attachment;
                }
                else
                {
                    attachment.clearValue.color.f =
                        {recipe.clear_color[0], recipe.clear_color[1], recipe.clear_color[2], recipe.clear_color[3]};
                    rendering.colors = &attachment;
                    rendering.colorNum = 1;
                }
                nri_core.CmdBeginRendering(command, rendering);
                nri_core.CmdEndRendering(command);
                break;
            }
            case ERecipe::FILL_BUFFER:
                nri_core.CmdZeroBuffer(
                    command,
                    *destination.buffer,
                    destination.buffer_offset,
                    std::get<GraphBufferDesc>(code.resources()[recipe.destination].description).size
                );
                break;
            case ERecipe::COPY_IMAGE_BUFFER:
            {
                const auto* texture = bindings[recipe.source].image;
                const auto& image = nri_core.GetTextureDesc(*texture);
                const auto format =
                    imageFormat(std::get<GraphImageDesc>(code.resources()[recipe.source].description).format);
                const nri::TextureRegionDesc region{0, 0, 0, image.width, image.height, 1, 0, 0, format.aspects};
                const nri::TextureDataLayoutDesc layout{
                    pass.copy_scratch ? 0 : destination.buffer_offset,
                    pass.copy_row_pitch,
                    pass.copy_slice_pitch
                };
                if (pass.copy_scratch)
                {
                    nri::BufferBarrierDesc scratch{
                        pass.copy_scratch,
                        {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY},
                        {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
                    };
                    const nri::BarrierDesc barrier{nullptr, 0, &scratch, 1};
                    nri_core.CmdBarrier(command, barrier);
                    nri_core.CmdReadbackTextureToBuffer(command, *pass.copy_scratch, layout, *texture, region);
                    std::swap(scratch.before, scratch.after);
                    nri_core.CmdBarrier(command, barrier);
                    const auto row_bytes = std::uint64_t(image.width) * format.bytes;
                    for (std::uint32_t row = 0; row < image.height; ++row)
                    {
                        nri_core.CmdCopyBuffer(
                            command,
                            *destination.buffer,
                            destination.buffer_offset + row * row_bytes,
                            *pass.copy_scratch,
                            std::uint64_t(row) * pass.copy_row_pitch,
                            row_bytes
                        );
                    }
                }
                else
                {
                    nri_core.CmdReadbackTextureToBuffer(command, *destination.buffer, layout, *texture, region);
                }
                break;
            }
            case ERecipe::COPY_BUFFER:
            {
                const auto& source = bindings[recipe.source];
                nri_core.CmdCopyBuffer(
                    command,
                    *destination.buffer,
                    destination.buffer_offset,
                    *source.buffer,
                    source.buffer_offset,
                    std::get<GraphBufferDesc>(code.resources()[recipe.source].description).size
                );
                break;
            }
            }
        }
    }

    void ExecutionContext::submitted(std::uint32_t slot) noexcept
    {
        progress_.last_submitted_slot = slot;
        ++progress_.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
    }
} // namespace lux::render::detail
