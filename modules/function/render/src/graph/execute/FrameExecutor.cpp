#include <algorithm>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
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

    RenderResult<void>
    FrameExecutor::recordBatchPart(
        const FrameRecordInput& frame,
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        std::uint32_t batch_index,
        bool output_tail,
        BindingState& binding_state
    ) noexcept
    {
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
            const auto& compiled = *pass.compiled_pass;
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
            const auto& program = pass.program_bindings;
            const auto* scope = compiled.rasterScope();
            const bool owns_draw_sections = scope && scope->exclusive;
            if (program.layout && !pass.native.draws && !owns_draw_sections)
            {
                const auto point = compiled.kind == EGraphPassKind::GRAPHICS
                    ? nri::BindPoint::GRAPHICS : nri::BindPoint::COMPUTE;
                if (binding_state.layout != program.layout || binding_state.point != point)
                {
                    core.CmdSetPipelineLayout(command, point, *program.layout);
                    binding_state.layout = program.layout;
                    binding_state.point = point;
                    binding_state.descriptors = {};
                }
                auto* pipeline = pass.pipeline;
                if (pipeline && binding_state.pipeline != pipeline)
                {
                    core.CmdSetPipeline(command, *pipeline);
                    binding_state.pipeline = pipeline;
                    binding_state.depth_bias.reset();
                }
                bindPassDescriptors(
                    core, frame.shared_descriptors, command, program,
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
            const PassRecordInput input{frame.resources, pass.parameters, &native};
            const bool managed_rendering = compiled.rendering_group != kGraphInvalidIndex && !owns_draw_sections;
            if (managed_rendering && native.begin_rendering)
            {
                nri::RenderingDesc rendering{};
                rendering.colors = native.colors.data();
                rendering.colorNum = static_cast<std::uint32_t>(native.colors.size());
                rendering.depth = native.depth;
                core.CmdBeginRendering(command, rendering);
            }
            if (managed_rendering)
            {
                if (native.reset_viewport || !binding_state.scissor)
                {
                    core.CmdSetViewports(command, &native.viewport, 1);
                    setPassScissor(core, command, native.scissor, binding_state);
                }
                if (native.dynamic_depth_bias || binding_state.depth_bias)
                    setPassDepthBias(core, command, native.depth_bias, binding_state);
            }
            if (owns_draw_sections) binding_state.invalidate();
            const auto recorded = recordBlock(plan.recording_, compiled.commands, [&](const auto& packet) noexcept
            {
                return packet.record(input, command, core);
            });
            if (owns_draw_sections || compiled.kind == EGraphPassKind::TRANSFER) binding_state.invalidate();
            if (managed_rendering && (native.end_rendering || !recorded)) core.CmdEndRendering(command);
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
