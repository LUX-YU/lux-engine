#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <bit>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/PassBindings.hpp>
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>

namespace lux::render::detail
{
    void
    FrameExecutor::recordHistoryInitialization(
        const FrozenRecordingFrame& input,
        const nri::CoreInterface& core,
        nri::CommandBuffer& command
    ) noexcept
    {
        for (const auto& history : input.history_initializations)
        {
            if (history.image_before.texture)
            {
                core.CmdBarrier(command, {nullptr, 0, nullptr, 0, &history.image_before, 1});
                for (auto* view : history.views)
                {
                    nri::AttachmentDesc attachment{};
                    attachment.descriptor = view;
                    attachment.loadOp = nri::LoadOp::CLEAR;
                    attachment.storeOp = nri::StoreOp::STORE;
                    attachment.clearValue = history.clear_value;
                    nri::RenderingDesc rendering{};
                    if (history.depth) rendering.depth = attachment;
                    else
                    {
                        rendering.colors = &attachment;
                        rendering.colorNum = 1;
                    }
                    core.CmdBeginRendering(command, rendering);
                    core.CmdEndRendering(command);
                }
                core.CmdBarrier(command, {nullptr, 0, nullptr, 0, &history.image_after, 1});
            }
            else
            {
                core.CmdBarrier(command, {nullptr, 0, &history.buffer_before, 1});
                core.CmdZeroBuffer(command, *history.buffer_before.buffer, 0, history.buffer_size);
                core.CmdBarrier(command, {nullptr, 0, &history.buffer_after, 1});
            }
        }
    }

    RenderResult<std::span<nri::CommandBuffer* const>>
    FrameExecutor::recordBatch(
        const FrozenRecordingFrame& frame, const DeviceSession& device, std::uint32_t batch
    ) noexcept
    {
        const bool invalid_batch = batch >= frame.revisions.size() || batch >= frame.native_batches.size() ||
            batch >= frame.plan.submissions().size();
        if (invalid_batch) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& recording = frame.revisions[batch];
        const auto& native = frame.native_batches[batch];
        if (recording.recording.exchange(true, std::memory_order_acquire))
            return cxx::unexpected(RenderError{kDeviceBusy});
        cxx::scope_exit release([&]() noexcept { recording.recording.store(false, std::memory_order_release); });
        const auto& description = frame.plan.submissions()[batch];
        const bool has_tail = description.output_begin != kGraphInvalidIndex;
        const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
        const std::uint32_t end_part = has_tail ? 2u : 1u;
        for (auto part = first_part; part < end_part; ++part)
        {
            const bool reusable = part == 0 && description.reusable_recording;
            if (reusable && recording.revision == recording.recorded_revision) continue;
            if (part == 0) recording.recorded_revision = 0;
            auto* command = native.commands[part];
            auto result = device.resetCommandAllocator(*native.allocators[part]);
            if (result == nri::Result::SUCCESS) result = device.beginCommandBuffer(*command, reusable);
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            recordBatchPart(frame, device.nriCore(), *command, {batch, part == 1});
            result = device.nriCore().EndCommandBuffer(*command);
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            if (reusable) recording.recorded_revision = recording.revision;
        }
        return std::span<nri::CommandBuffer* const>{
            native.commands.data() + first_part, end_part - first_part
        };
    }

    void FrameExecutor::recordBatchPart(
        const FrozenRecordingFrame& frame,
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        BatchPart part
    ) noexcept
    {
        const auto [batch_index, output_tail] = part;
        BindingState binding_state;
        const RasterEncoder raster{core, command, frame, binding_state};
        const auto& plan = frame.plan;
        const auto& batch = plan.submissions_[batch_index];
        const bool has_tail = batch.output_begin != kGraphInvalidIndex;
        const bool first_part = !output_tail || batch.output_begin == batch.first_pass;
        const bool last_part = output_tail || !has_tail;
        if (batch_index == 0 && first_part)
            recordHistoryInitialization(frame, core, command);
        const auto first_pass = output_tail ? batch.output_begin : batch.first_pass;
        const auto end_pass = !output_tail && has_tail ? batch.output_begin : batch.first_pass + batch.pass_count;
        for (std::size_t p = first_pass; p < end_pass; ++p)
        {
            const auto& pass = frame.passes[p];
            const auto& compiled = plan.passes()[p];
            if (pass.alias_barrier)
            {
                // Establish the alias memory dependency before any new image layout transition.
                core.CmdBarrier(command, {&*pass.alias_barrier, 1});
            }
            if (!pass.images.empty() || !pass.buffers.empty())
            {
                const nri::BarrierDesc barrier{
                    nullptr,
                    0,
                    pass.buffers.data(),
                    static_cast<std::uint32_t>(pass.buffers.size()),
                    pass.images.data(),
                    static_cast<std::uint32_t>(pass.images.size())
                };
                core.CmdBarrier(command, barrier);
            }
            // Both paths reach the compiled entry state. False preserves the validated predecessor contents.
            if (compiled.condition_region != kGraphInvalidIndex && !frame.enabled_regions[compiled.condition_region])
            {
                continue;
            }
            const auto& delta = compiled.binding_join == kGraphInvalidIndex ? compiled.fixed_bindings :
                frame.binding_choices[compiled.binding_join].commands;
            const auto& program = plan.programs()[compiled.binding_slot];
            if (delta.pool && frame.descriptor_pool) core.CmdSetDescriptorPool(command, *frame.descriptor_pool);
            const auto& binding = program.descriptors;
            if (delta.layout) core.CmdSetPipelineLayout(command, delta.point, *binding.layout);
            if (delta.pipeline)
            {
                core.CmdSetPipeline(command, *program.pipeline);
                binding_state.depth_bias.reset();
            }
            for (auto mask = delta.shared_sets; mask; mask &= mask - 1)
            {
                const auto kind = std::countr_zero(mask);
                const auto* selected = frame.shared_descriptors[kind];
                const nri::SetDescriptorSetDesc set{binding.shared_sets[kind], selected, delta.point};
                core.CmdSetDescriptorSet(command, set);
            }
            if (delta.private_set)
                core.CmdSetDescriptorSet(command, {binding.private_set, pass.private_descriptors, delta.point});
            if (delta.constants)
            {
                const auto bytes = pass.parameters;
                const auto size = static_cast<std::uint32_t>(bytes.size());
                const nri::SetRootConstantsDesc block{0, bytes.data(), size, 0, delta.point};
                core.CmdSetRootConstants(command, block);
            }
            const auto* scope = plan.recordingProgram().rasterScope(compiled);
            const bool owns_draw_sections = scope && scope->exclusive;
            const auto* group = scope && !owns_draw_sections ? &plan.recording_.groups[compiled.rendering_group] : nullptr;
            if (group && group->first_pass == p)
            {
                nri::RenderingDesc rendering{};
                rendering.colors = pass.colors.data();
                rendering.colorNum = static_cast<std::uint32_t>(pass.colors.size());
                rendering.depth = pass.depth;
                core.CmdBeginRendering(command, rendering);
            }
            if (group)
            {
                if (scope->reset_viewport || !binding_state.scissor)
                {
                    core.CmdSetViewports(command, &pass.viewport, 1);
                    setPassScissor(core, command, pass.scissor, binding_state);
                }
                if (binding_state.depth_bias)
                    setPassDepthBias(core, command, scope->depth_bias, binding_state);
            }
            if (owns_draw_sections) binding_state.invalidate();
            recordBlock(plan.recording_, compiled.commands, [&](const auto& packet, std::uint32_t row) noexcept
            {
                using Packet = std::remove_cvref_t<decltype(packet)>;
                if constexpr (std::same_as<Packet, RasterCommands>) raster.record(packet, pass);
                else if constexpr (std::same_as<Packet, ComputeCommands>)
                {
                    binding_state.vertices = {};
                    binding_state.index = nullptr;
                    const auto& dispatch = frame.dispatches[row];
                    if (dispatch.indirect_buffer)
                        core.CmdDispatchIndirect(command, *dispatch.indirect_buffer, dispatch.indirect_offset);
                    else core.CmdDispatch(command, dispatch.dispatch);
                }
                else recordTransfers(packet, frame, command, core);
            });
            const bool clobbers = owns_draw_sections || std::holds_alternative<TransferCommandSlot>(compiled.commands);
            if (clobbers) binding_state.invalidate();
            if (group && group->first_pass + group->pass_count == p + 1) core.CmdEndRendering(command);
        }
        if (last_part && batch_index + 1 == plan.submissions_.size() &&
            (!frame.final_images.empty() || !frame.final_buffers.empty()))
        {
            const nri::BarrierDesc barrier{
                nullptr,
                0,
                frame.final_buffers.data(),
                static_cast<std::uint32_t>(frame.final_buffers.size()),
                frame.final_images.data(),
                static_cast<std::uint32_t>(frame.final_images.size())
            };
            core.CmdBarrier(command, barrier);
        }
    }

} // namespace lux::render::detail
