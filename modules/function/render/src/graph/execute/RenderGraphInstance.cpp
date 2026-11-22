#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <algorithm>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/graph/execute/RasterFramePreparation.hpp>

namespace lux::render::detail
{
    RenderGraphInstance::RenderGraphInstance(
        JobGpuResources& gpu,
        const compiled::RenderGraph& plan,
        FrameStorage storage,
        TargetOutput output
    ) noexcept
        : program_inputs_(storage.programs), preparation_job_(storage.binding.job), preparation_generation_(storage.binding.generation),
          session_(&gpu.session), gpu_(gpu), frames_(storage.frames), histories_(storage.histories),
          graph_(plan), output_(output)
    {
        condition_values_.resize(plan.condition_count_);
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

    RenderGraphInstance::~RenderGraphInstance() noexcept
    {
        for (const auto& frame : frames_)
            for (const auto& batch : frame.command_batches)
                if (batch.recording.load(std::memory_order_acquire)) std::terminate();
        while (!producers_.empty()) producers_.pop_back();
    }

    RenderResult<void> RenderGraphInstance::initialize() noexcept
    {
        if (binding_values_.size() != graph_.binding_predicate_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& session = *session_;
        const auto& plan = graph_;
        const bool wrong_device = plan.device() != &session.device();
        const bool wrong_target =
            !output_.frames.empty() &&
            (!plan.compatible(output_.description) || output_.frames.size() != frames_.size());
        const bool needs_target = plan.output() != kGraphInvalidIndex ||
            std::ranges::any_of(plan.resources(), [](const auto& resource) noexcept { return resource.target_sized; });
        const bool missing_target = needs_target && output_.frames.empty();
        const bool invalid_programs = plan.programs().size() != plan.passes().size();
        const bool missing_programs = invalid_programs || std::ranges::any_of(plan.passes(), [&](const auto& pass)
        {
            const auto& bindings = plan.binding_plan_->pass(pass.binding_slot);
            const auto& native = graph_.programs()[pass.binding_slot];
            if (bindings.has_private_set && !native.descriptors.layout) return true;
            if (const auto* raster = plan.rasterCommands(pass))
                return !std::holds_alternative<RasterListCommands>(raster->draw) && !native.pipeline;
            const auto* compute = plan.computeCommands(pass);
            return compute && !native.pipeline;
        });
        if (wrong_device || wrong_target || missing_target || missing_programs)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (auto histories = createHistories(); !histories)
        {
            return histories;
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            frames_[slot].initializeRasterStreams(graph_.recordingDependencies());
            if (auto resources = createFrameResources(slot); !resources)
            {
                return resources;
            }
            if (auto passes = prepareFrameBindings(slot); !passes)
            {
                return passes;
            }
            prepareAliasBarriers(frames_[slot]);
            frames_[slot].command_batches.resize(plan.submissions().size());
            if (auto recordings = frames_[slot].native.createBatches(session, plan.submissions()); !recordings)
            {
                return recordings;
            }
        }
        if (auto descriptors = prepareDescriptors(); !descriptors)
        {
            return descriptors;
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            frames_[slot].linkRecordInputs(plan);
            frames_[slot].bindHistory(histories_, graph_.recordingDependencies());
            frames_[slot].updateHistoryViews(histories_, session_->nriCore());
        }
        return {};
    }

    bool RenderGraphInstance::conditionEnabled(const FrameResources& frame, PassCondition condition) const noexcept
    {
        switch (condition.source)
        {
        case EConditionSource::HISTORY_VALID: return bool(frame.history_valid[condition.input]) == condition.expected;
        case EConditionSource::SAME_BINDING: return bool(binding_values_[condition.input]) == condition.expected;
        case EConditionSource::INPUT: return bool(frame.conditions[condition.input]) == condition.expected;
        }
        std::terminate();
    }

    bool RenderGraphInstance::frozenCondition(std::uint32_t slot, PassCondition condition) const noexcept
    {
        return conditionEnabled(frames_[slot], condition);
    }

    RenderResult<void> RenderGraphInstance::bindPredicates(std::vector<std::uint8_t> selected) noexcept
    {
        if (selected.size() != graph_.binding_predicate_count_ || !binding_values_.empty())
            return cxx::unexpected(RenderError{kDeviceArgument});
        binding_values_ = std::move(selected);
        return {};
    }

    RenderResult<void> RenderGraphInstance::setConditions(std::span<const std::uint8_t> values) noexcept
    {
        const bool is_wrong_count = values.size() != condition_values_.size();
        const bool has_invalid_value = std::ranges::any_of(values, [](auto value) { return value > 1; });
        if (is_wrong_count || has_invalid_value) return cxx::unexpected(RenderError{kDeviceArgument});
        std::ranges::copy(values, condition_values_.begin());
        return {};
    }

    RenderResult<bool> RenderGraphInstance::prepareFrame(std::uint32_t slot) noexcept
    {
        if (const auto error = session_->terminalError(); error.type) return cxx::unexpected(error);
        if (slot >= frames_.size()) return cxx::unexpected(RenderError{kDeviceArgument});
        frames_[slot].record_ready = false;
        if (submission_error_.type != 0)
        {
            return cxx::unexpected(submission_error_);
        }
        auto& frame = frames_[slot];
        if (graph_.output() != kGraphInvalidIndex)
        {
            const auto& output = output_.frames[slot];
            auto& binding = frame.bindings[graph_.output()];
            if (binding.image != output.image)
            {
                retireRasterViews(frame, nullptr, binding.image);
                frame.invalidateResource(graph_.recordingDependencies(), graph_.output());
                binding.select(nullptr, output.image, session_->nriCore());
                // A presentation image has one writer and cannot be sampled by another instance.
                // Rotation changes this binding and its barriers, not the cross-instance dependency graph.
                if (output_.description.kind != ETargetKind::SURFACE)
                {
                    frame.invalidateImportedBindings();
                }
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(graph_.output());
                }
                for (auto* view : frame.output_views)
                {
                    *view = output.view;
                }
            }
        }
        if (frame.unavailable_buffers)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!histories_.empty())
        {
            if (!selectHistory()) return false;
            frame.bindHistory(histories_, graph_.recordingDependencies());
        }
        std::ranges::copy(condition_values_, frame.conditions.begin());
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            const auto& sample = histories_[i].plan->description;
            const bool enabled = !sample.publish_when || conditionEnabled(frame, *sample.publish_when);
            const bool produced = !sample.select_primary || conditionEnabled(frame, *sample.select_primary);
            frame.publish_history[i] = enabled && produced;
        }
        auto changed_join = kGraphInvalidIndex;
        for (std::size_t r = 0; r < graph_.recording_.conditions.size(); ++r)
        {
            const auto& region = graph_.recording_.conditions[r];
            const bool enabled = conditionEnabled(frame, {region.source, region.input_slot, region.expected});
            const bool changed = (frame.enabled_regions[r] != 0) != enabled;
            if (changed)
            {
                if (enabled) frame.enabled_pass_count += region.pass_count;
                else frame.enabled_pass_count -= region.pass_count;
                for (auto p = region.first_pass; p < region.first_pass + region.pass_count; ++p)
                {
                    frame.invalidatePass(graph_, p);
                    if (!enabled) continue;
                    const auto& offsets = graph_.recordingDependencies().pass_field_offsets;
                    for (auto i = offsets[p]; i < offsets[p + 1]; ++i) frame.pending_operand_fields.mark(i);
                }
            }
            const auto join = region.binding_join;
            if (join != kGraphInvalidIndex)
            {
                const auto fallback = graph_.recording_.binding_joins[join].disabled_join;
                const bool inherited = !enabled && fallback != kGraphInvalidIndex && fallback == changed_join;
                if ((changed || inherited) && frame.selectBindingJoin(graph_, join, enabled)) changed_join = join;
            }
            frame.enabled_regions[r] = enabled;
        }
        if (!frame.changed_resources.empty())
        {
            if (auto rebound = rebind(slot); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        if (!frame.pending_history_views.slots().empty()) frame.updateHistoryViews(histories_, session_->nriCore());
        if (frame.viewport != viewport_)
        {
            for (const auto p : graph_.recordingDependencies().viewport_passes) frame.invalidatePass(graph_, p);
            frame.viewport = viewport_;
        }
        const auto& shared = session_->descriptors();
        const auto shared_generation = shared.nativeBindingGeneration();
        const bool is_d3d12 = session_->description().graphicsAPI == nri::GraphicsAPI::D3D12;
        const bool shared_changed = frame.shared_descriptors.generation != shared_generation;
        const bool resize_pool = shared_changed && frame.shared_descriptors.capacities != shared.snapshotCounts();
        RasterFramePreparation raster(graph_, frame, gpu_);
        if (resize_pool || !frame.stream_inputs.empty())
        {
            if (auto capacity = raster.prepareDescriptorCapacity(resize_pool); !capacity)
                return cxx::unexpected(capacity.error());
        }
        if (is_d3d12 && frame.native.descriptor_pool)
        {
            if (auto synced = shared.updateSnapshot(frame.shared_descriptors); !synced)
                return cxx::unexpected(synced.error());
        }
        else if (!is_d3d12 && shared_changed)
        {
            constexpr std::array kinds{
                nri::DescriptorType::TEXTURE, nri::DescriptorType::SAMPLER, nri::DescriptorType::STRUCTURED_BUFFER
            };
            const auto& dependencies = graph_.recordingDependencies();
            for (std::size_t kind = 0; kind < kinds.size(); ++kind)
            {
                const auto generation = shared.nativeSetGeneration(kinds[kind]);
                if (frame.shared_set_generations[kind] == generation) continue;
                frame.shared_set_generations[kind] = generation;
                frame.shared_descriptors.sets[kind] = shared.descriptors(kinds[kind]);
                for (auto batch : dependencies.shared_batches[kind]) frame.command_batches[batch].invalidate();
                for (const auto list : dependencies.raster_lists)
                {
                    const auto uses_set = [kind](const RasterDrawPacket& draw) noexcept
                    {
                        return draw.bindings.shared_sets[kind] != kGraphInvalidIndex;
                    };
                    if (std::ranges::any_of(frame.raster_draws[list.list].packets, uses_set))
                        frame.command_batches[list.batch].invalidate();
                }
            }
            frame.shared_descriptors.generation = shared_generation;
        }
        if (!frame.stream_inputs.empty())
        {
            if (auto sections = raster.prepareSections(); !sections) return cxx::unexpected(sections.error());
            if (auto streams = raster.materializeStreams(); !streams) return cxx::unexpected(streams.error());
        }
        auto frozen = frame.freeze(histories_, session_->description());
        if (!frozen) return cxx::unexpected(frozen.error());
        return true;
    }

    void RenderGraphInstance::failedSubmission(RenderError error) noexcept
    {
        submission_error_ = error;
    }

    void RenderGraphInstance::submitted(std::uint32_t slot) noexcept
    {
        publishHistory(slot);
        progress_.last_submitted_slot = slot;
        ++progress_.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
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

    std::uint32_t RenderGraphInstance::recordedPassCount(std::uint32_t slot) const noexcept
    {
        return frames_[slot].enabled_pass_count;
    }
} // namespace lux::render::detail
