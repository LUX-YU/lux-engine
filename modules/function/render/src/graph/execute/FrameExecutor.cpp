#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <algorithm>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/graph/execute/FrameRecordInput.hpp>
#include <lux/engine/render/graph/execute/PassBindings.hpp>
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>

namespace lux::render::detail
{
    void
    FrameExecutor::recordHistoryInitialization(
        const FrameRecordInput& input,
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
            batch >= frame.input.plan.submissions().size();
        if (invalid_batch) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& recording = frame.revisions[batch];
        const auto& native = frame.native_batches[batch];
        if (recording.recording.exchange(true, std::memory_order_acquire))
            return cxx::unexpected(RenderError{kDeviceBusy});
        cxx::scope_exit release([&]() noexcept { recording.recording.store(false, std::memory_order_release); });
        const auto& description = frame.input.plan.submissions()[batch];
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
            auto encoded = recordBatchPart(frame.input, device.nriCore(), *command, {batch, part == 1});
            result = device.nriCore().EndCommandBuffer(*command);
            if (!encoded) return cxx::unexpected(encoded.error());
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            if (reusable) recording.recorded_revision = recording.revision;
        }
        return std::span<nri::CommandBuffer* const>{
            native.commands.data() + first_part, end_part - first_part
        };
    }

    RenderResult<void>
    FrameExecutor::recordBatchPart(
        const FrameRecordInput& frame,
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        BatchPart part
    ) noexcept
    {
        const auto [batch_index, output_tail] = part;
        BindingState binding_state;
        const auto& plan = frame.plan;
        const auto& batch = plan.submissions_[batch_index];
        // Logical vertex-resource indices are instance-local. Pipeline and descriptor state
        // use physical identities and may be retained across the whole merged recording.
        binding_state.vertices = {};
        const bool has_tail = batch.output_begin != kGraphInvalidIndex;
        const bool first_part = !output_tail || batch.output_begin == batch.first_pass;
        const bool last_part = output_tail || !has_tail;
        if (batch_index == 0 && first_part)
        {
            recordHistoryInitialization(frame, core, command);
            if (!frame.history_initializations.empty())
            {
                binding_state = {};
            }
        }
        const auto first_pass = output_tail ? batch.output_begin : batch.first_pass;
        const auto end_pass = !output_tail && has_tail ? batch.output_begin : batch.first_pass + batch.pass_count;
        for (std::size_t p = first_pass; p < end_pass; ++p)
        {
            const auto& pass = frame.passes[p];
            const auto& compiled = plan.passes()[p];
            const auto& parameters = pass.parameters;
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
            if (frame.descriptor_pool && binding_state.descriptor_pool != frame.descriptor_pool)
            {
                core.CmdSetDescriptorPool(command, *frame.descriptor_pool);
                binding_state.descriptor_pool = frame.descriptor_pool;
                binding_state.descriptors = {};
            }
            const auto& program = frame.plan.programs()[compiled.binding_slot];
            const auto* scope = plan.recordingProgram().rasterScope(compiled);
            const bool owns_draw_sections = scope && scope->exclusive;
            if (program.descriptors.layout && !pass.native.draws && !owns_draw_sections)
            {
                const auto point = scope != nullptr
                    ? nri::BindPoint::GRAPHICS : nri::BindPoint::COMPUTE;
                if (binding_state.layout != program.descriptors.layout || binding_state.point != point)
                {
                    core.CmdSetPipelineLayout(command, point, *program.descriptors.layout);
                    binding_state.layout = program.descriptors.layout;
                    binding_state.point = point;
                    binding_state.descriptors = {};
                }
                auto* pipeline = program.pipeline;
                if (pipeline && binding_state.pipeline != pipeline)
                {
                    core.CmdSetPipeline(command, *pipeline);
                    binding_state.pipeline = pipeline;
                    binding_state.depth_bias.reset();
                }
                bindPassDescriptors(
                    core, frame.shared_descriptors, command, program.descriptors,
                    {&pass.private_descriptors, pass.private_descriptors ? 1u : 0u}, binding_state
                );
                if (!parameters.empty())
                {
                    core.CmdSetRootConstants(
                        command,
                        {0, parameters.data(), static_cast<std::uint32_t>(parameters.size()), 0, point}
                    );
                }
            }
            auto native = pass.native;
            native.bindings = &binding_state;
            const PassRecordInput input{frame.resources, frame.shared_descriptors, &native};
            const auto* group = scope && !owns_draw_sections ? &plan.recording_.groups[compiled.rendering_group] : nullptr;
            if (group && group->first_pass == p)
            {
                nri::RenderingDesc rendering{};
                rendering.colors = native.colors.data();
                rendering.colorNum = static_cast<std::uint32_t>(native.colors.size());
                rendering.depth = native.depth;
                core.CmdBeginRendering(command, rendering);
            }
            if (group)
            {
                if (scope->reset_viewport || !binding_state.scissor)
                {
                    core.CmdSetViewports(command, &native.viewport, 1);
                    setPassScissor(core, command, native.scissor, binding_state);
                }
                if (binding_state.depth_bias)
                    setPassDepthBias(core, command, scope->depth_bias, binding_state);
            }
            if (owns_draw_sections) binding_state.invalidate();
            const auto recorded = recordBlock(plan.recording_, compiled.commands, [&](const auto& packet) noexcept
            {
                using Packet = std::remove_cvref_t<decltype(packet)>;
                if constexpr (std::same_as<Packet, RasterCommands>) return packet.record(input, command, core);
                else
                {
                    if constexpr (std::same_as<Packet, ComputeCommands>) recordCompute(native, command, core);
                    else recordTransfers(packet, frame, command, core);
                    return RenderResult<void>{};
                }
            });
            if (owns_draw_sections || std::holds_alternative<TransferCommandSlot>(compiled.commands)) binding_state.invalidate();
            if (group && (group->first_pass + group->pass_count == p + 1 || !recorded)) core.CmdEndRendering(command);
            if (!recorded) return recorded;
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
        return {};
    }

} // namespace lux::render::detail
