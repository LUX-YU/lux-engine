#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <algorithm>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>

namespace lux::render::detail
{
    RenderGraphInstance::RenderGraphInstance(
        JobGpuResources& gpu,
        const compiled::RenderGraph& plan,
        FrameStorage storage,
        TargetOutput& output
    ) noexcept
        : session_(&gpu.session), gpu_(gpu), frames_(storage.frames), histories_(storage.histories),
          graph_(plan), output_(output)
    {
        const auto parameter_bytes = plan.passes().empty() ? 0 :
            plan.passes().back().parameter_offset + plan.passes().back().parameter_size;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            frame.invalidateImportedBindings();
            frame.parameters.resize(parameter_bytes);
            for (const auto& pass : plan.passes())
            {
                const auto& initial = plan.binding_plan_->pass(pass.binding_slot).initial_parameters;
                std::ranges::copy(initial, frame.parameters.begin() + pass.parameter_offset);
            }
            frame.bindings.resize(plan.resources().size());
            frame.record_resources.resize(plan.resources().size());
            frame.record_passes.resize(plan.passes().size());
            frame.pending_resources.initialize(plan.resources().size());
            frame.pending_passes.initialize(plan.passes().size());
            frame.pending_operand_fields.initialize(plan.binding_plan_->recordings.operand_fields.size());
            frame.transfers.initialize(plan.recordingProgram().transfers);
            const auto clear_capacity = plan.histories_.size() * 2;
            frame.history_initializations.prepareCapacity(clear_capacity, clear_capacity);
            frame.pending_history_clears.initialize(plan.histories_.size());
            frame.pending_history_views.initialize(plan.histories_.size());
            frame.enabled_regions.resize(plan.recording_.conditions.size(), 1);
            frame.enabled_pass_count = static_cast<std::uint32_t>(plan.passes().size());
            frame.history_valid.resize(plan.histories_.size(), 0);
            frame.conditions.resize(plan.condition_count_);
            frame.publish_history.resize(plan.histories_.size());
            frame.history_clears.resize(plan.histories_.size());
            frame.changed_resources.reserve(plan.resources().size());
            frame.unavailable_buffers = std::ranges::count_if(
                plan.resources(),
                [](const auto& resource)
                {
                    const bool runtime_bound = resource.origin == EGraphOrigin::IMPORTED ||
                        resource.origin == EGraphOrigin::FRAME_BUFFER;
                    return resource.memory_used && std::holds_alternative<GraphBufferDesc>(resource.description) &&
                        runtime_bound;
                }
            );
            frame.native.images.reserve(std::ranges::count_if(
                plan.resources(),
                [](const auto& resource)
                {
                    return resource.memory_used && std::holds_alternative<GraphImageDesc>(resource.description) &&
                           resource.origin != EGraphOrigin::IMPORTED;
                }
            ));
        }
    }

    std::uint32_t RenderGraphInstance::frameCount() const noexcept
    { return static_cast<std::uint32_t>(frames_.size()); }

    bool RenderGraphInstance::frozenForSubmission(std::uint32_t slot) const noexcept
    {
        return slot < frames_.size() && frames_[slot].frozen_for_submission;
    }

    RenderGraphInstance::~RenderGraphInstance() noexcept
    {
        for (const auto& frame : frames_)
            for (const auto& batch : frame.command_batches)
                if (batch.recording.load(std::memory_order_acquire)) std::terminate();
    }

    RenderResult<std::span<nri::CommandBuffer* const>>
    RenderGraphInstance::recordBatch(std::uint32_t frame_slot, std::uint32_t batch) noexcept
    {
        if (const auto error = session_->terminalError(); error.type) return cxx::unexpected(error);
        const auto* frame = recordingFrame(frame_slot);
        if (!frame) return cxx::unexpected(RenderError{kDeviceArgument});
        return FrameExecutor::recordBatch(*frame, *session_, batch);
    }

    const FrozenRecordingFrame* RenderGraphInstance::recordingFrame(std::uint32_t frame_slot) const & noexcept
    {
        return frame_slot < frames_.size() ? frames_[frame_slot].recordingFrame() : nullptr;
    }

    void RenderGraphInstance::invalidateRasterDraws(const CompiledPass& pass, std::uint32_t frame_slot) noexcept
    {
        frames_[frame_slot].invalidatePass(graph_, pass.binding_slot);
    }

    RasterDrawList&
    RenderGraphInstance::rasterDraws(const CompiledPass& pass, std::uint32_t slot, bool recording_changed) noexcept
    {
        if (recording_changed) invalidateRasterDraws(pass, slot);
        const auto& raster = *graph_.rasterCommands(pass);
        return frames_[slot].raster_draws[std::get<RasterListCommands>(raster.draw).data_index];
    }

    FrozenOperandPatches RenderGraphInstance::lastOperandPatches(std::uint32_t slot) const noexcept
    {
        return frames_[slot].last_operands;
    }

    FrozenPatchCounts RenderGraphInstance::lastFrozenPatches(std::uint32_t slot) const noexcept
    {
        return frames_[slot].last_patches;
    }

} // namespace lux::render::detail
