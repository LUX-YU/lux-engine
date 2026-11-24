#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <algorithm>
#include <array>

namespace lux::render::runtime_detail
{
    using detail::FrameResources;
    using detail::JobHistoryResources;
    using detail::RasterDrawPacket;
    using detail::RasterListCommands;
    using detail::SubmissionPoint;

    RenderResult<void> JobExecution::initialize() noexcept
    {
        if (binding_values_.size() != graph().bindingPredicateCount())
            return cxx::unexpected(RenderError{kDeviceArgument});
        auto& session = gpu_.session;
        const auto& plan = graph();
        const bool wrong_device = plan.device() != &session.device();
        const bool wrong_target =
            !output_.frames.empty() &&
            (!plan.compatible(output_.description.graphRequirements()) || output_.frames.size() != frame_bindings.frames.size());
        const bool needs_target = plan.output() != kGraphInvalidIndex ||
            std::ranges::any_of(plan.resources(), [](const auto& resource) noexcept { return resource.target_sized; });
        const bool missing_target = needs_target && output_.frames.empty();
        const bool invalid_programs = plan.programs().size() != plan.passes().size();
        const bool missing_programs = invalid_programs || std::ranges::any_of(plan.passes(), [&](const auto& pass)
        {
            const auto& bindings = plan.bindingPlan().pass(pass.binding_slot);
            const auto& native = plan.programs()[pass.binding_slot];
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
        gpu_.histories.resize(histories_.size());
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            const auto& history = frame_bindings.graph.histories()[i];
            const auto distance = history.description.distance;
            if (frame_bindings.frames.size() > kGraphInvalidIndex - distance) return cxx::unexpected(RenderError{kDeviceCapacity});
            auto& bank = gpu_.histories[i];
            bank.slots.resize(frame_bindings.frames.size() + distance);
            const auto& resource = frame_bindings.graph.resources()[history.description.current.index];
            if (auto created = bank.initialize(session, history, resource, output_.description); !created) return created;
            histories_[i].bindBank(history, bank);
        }
        for (std::uint32_t slot = 0; slot < frame_bindings.frames.size(); ++slot)
        {
            const auto* output = output_.frames.empty() ? nullptr : &output_.frames[slot];
            if (auto installed = gpu_.installFrame(frame_bindings, slot, histories_, output); !installed) return installed;
        }
        auto pool = plan.bindingPlan().private_pool;
        if (auto counted = session.descriptors().addSnapshotDescriptors(pool); !counted) return counted;
        for (std::uint32_t slot = 0; slot < frame_bindings.frames.size(); ++slot)
        {
            if (auto descriptors = frame_bindings.frames[slot].createDescriptors(plan, session, pool); !descriptors) return descriptors;
            frame_bindings.frames[slot].linkRecordInputs(plan);
            frame_bindings.frames[slot].bindHistory(histories_, plan.recordingDependencies());
            frame_bindings.frames[slot].updateHistoryViews(histories_, session.nriCore());
        }
        return {};
    }

    bool JobExecution::conditionEnabled(PassCondition condition, const FrameResources* frozen) const noexcept
    {
        const auto input = condition.input;
        switch (condition.source)
        {
        case EConditionSource::HISTORY_VALID:
            return (frozen ? frozen->history_valid[input] != 0 : histories_[input].hasPrevious()) == condition.expected;
        case EConditionSource::SAME_BINDING: return bool(binding_values_[input]) == condition.expected;
        case EConditionSource::INPUT:
            return bool(frozen ? frozen->conditions[input] : condition_values_[input]) == condition.expected;
        }
        std::terminate();
    }

    RenderResult<void> JobExecution::invalidateHistory(std::span<const GraphResourceId> previous) noexcept
    {
        // Validate the whole request before changing any instance state.
        for (auto resource : previous)
        {
            const bool valid = resource.index < frame_bindings.graph.resources().size() &&
                               frame_bindings.graph.resources()[resource.index].origin == EGraphOrigin::HISTORY_PREVIOUS &&
                               frame_bindings.graph.resources()[resource.index].history != kGraphInvalidIndex;
            if (!valid)
            {
                return cxx::unexpected(RenderError{kGraphUnknownResource});
            }
        }
        if (previous.empty())
        {
            for (auto& history : histories_)
            {
                history.invalidateSamples();
            }
        }
        else
        {
            for (auto resource : previous)
            {
                histories_[frame_bindings.graph.resources()[resource.index].history].invalidateSamples();
            }
        }
        return {};
    }

    bool JobExecution::passEnabledThisRun(std::uint32_t pass) const noexcept
    {
        const auto index = frame_bindings.graph.passes()[pass].condition_region;
        if (index == kGraphInvalidIndex) return true;
        const auto& region = frame_bindings.graph.recordingProgram().conditions[index];
        return conditionEnabled({region.source, region.input_slot, region.expected});
    }

    bool JobExecution::writesTargetThisRun() const noexcept
    {
        if (frame_bindings.graph.writesOutputUnconditionally()) return true;
        for (const auto index : frame_bindings.graph.conditionalOutputWrites())
        {
            const auto& region = frame_bindings.graph.recordingProgram().conditions[index];
            if (conditionEnabled({region.source, region.input_slot, region.expected})) return true;
        }
        return false;
    }

    bool JobExecution::frozenCondition(std::uint32_t slot, PassCondition condition) const noexcept
    {
        return conditionEnabled(condition, &frame_bindings.frames[slot]);
    }

    RenderResult<void> JobExecution::bindPredicates(std::vector<std::uint8_t> selected) noexcept
    {
        if (selected.size() != graph().bindingPredicateCount() || !binding_values_.empty())
            return cxx::unexpected(RenderError{kDeviceArgument});
        binding_values_ = std::move(selected);
        return {};
    }

    RenderResult<void> JobExecution::setConditions(std::span<const std::uint8_t> values) noexcept
    {
        const bool is_wrong_count = values.size() != condition_values_.size();
        const bool has_invalid_value = std::ranges::any_of(values, [](auto value) { return value > 1; });
        if (is_wrong_count || has_invalid_value) return cxx::unexpected(RenderError{kDeviceArgument});
        for (std::uint32_t i = 0; i < values.size(); ++i)
        {
            if (values[i] == condition_values_[i]) continue;
            condition_values_[i] = values[i];
            for (auto& frame : frame_bindings.frames) frame.pending_condition_inputs.mark(i);
        }
        return {};
    }

    void JobExecution::freezeConditions(FrameResources& frame) const noexcept
    {
        const auto& plan = graph();
        const auto& dependencies = plan.recordingDependencies();
        for (auto input : frame.pending_condition_inputs.slots())
        {
            if (frame.conditions[input] == condition_values_[input]) continue;
            frame.conditions[input] = condition_values_[input];
            for (auto region : dependencies.input_regions[input]) frame.pending_regions.mark(region);
        }
        frame.pending_condition_inputs.clear();

        auto pending = frame.pending_regions.orderedSlots();
        const auto& recording = plan.recordingProgram();
        auto inherited = kGraphInvalidIndex;
        while (!pending.empty() || inherited != kGraphInvalidIndex)
        {
            // A disabled region can inherit a binding choice from the join at its entry.
            // Visit that successor in command order alongside regions whose predicate changed.
            const auto direct = pending.empty() ? kGraphInvalidIndex : pending.front();
            const auto index = std::min(direct, inherited);
            const bool revisit_join = index == inherited;
            if (index == direct) pending = pending.subspan(1);
            if (revisit_join) inherited = kGraphInvalidIndex;
            const auto& region = recording.conditions[index];
            const bool enabled = conditionEnabled({region.source, region.input_slot, region.expected}, &frame);
            const bool changed = (frame.enabled_regions[index] != 0) != enabled;
            if (changed)
            {
                frame.enabled_regions[index] = enabled;
                if (enabled) frame.enabled_pass_count += region.pass_count;
                else frame.enabled_pass_count -= region.pass_count;
                const auto end = region.first_pass + region.pass_count;
                // Scheduling and the WSI split keep the entire region in one native command buffer.
                const auto batch = plan.passes()[region.first_pass].submission;
                if (plan.submissions()[batch].reusesPass(region.first_pass)) frame.command_batches[batch].invalidate();
                if (enabled)
                {
                    const auto& offsets = dependencies.pass_field_offsets;
                    for (auto i = offsets[region.first_pass]; i < offsets[end]; ++i) frame.pending_operand_fields.mark(i);
                }
            }
            const auto join = region.binding_join;
            if (join == kGraphInvalidIndex || !(changed || (revisit_join && !enabled))) continue;
            if (frame.selectBindingJoin(plan, join, enabled))
                inherited = plan.passes()[recording.binding_joins[join].pass].condition_region;
        }
        frame.pending_regions.clear();
    }

    RenderResult<bool> JobExecution::freezeFrame(std::uint32_t slot, std::vector<detail::RasterDrawInput>& draws) noexcept
    {
        auto& session = gpu_.session;
        const auto& plan = graph();
        if (const auto error = session.terminalError(); error.type) return cxx::unexpected(error);
        if (slot >= frame_bindings.frames.size()) return cxx::unexpected(RenderError{kDeviceArgument});
        frame_bindings.frames[slot].record_ready = false;
        if (submission_error_.type != 0)
        {
            return cxx::unexpected(submission_error_);
        }
        auto& frame = frame_bindings.frames[slot];
        if (plan.output() != kGraphInvalidIndex)
        {
            const auto& output = output_.frames[slot];
            auto& binding = frame.bindings[plan.output()];
            if (binding.image != output.image)
            {
                frame.retireRasterViews(session, nullptr, binding.image);
                frame.invalidateResource(plan.recordingDependencies(), plan.output());
                binding.select(nullptr, output.image, session.nriCore());
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(plan.output());
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
            for (auto& history : histories_)
                if (!history.select(session)) return false;
            frame.bindHistory(histories_, plan.recordingDependencies());
        }
        freezeConditions(frame);
        if (!frame.changed_resources.empty())
        {
            if (auto rebound = frame.rebind(plan, session); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        if (!frame.pending_history_views.slots().empty()) frame.updateHistoryViews(histories_, session.nriCore());
        if (frame.viewport != viewport)
        {
            for (const auto p : plan.recordingDependencies().viewport_passes) frame.invalidatePass(plan, p);
            frame.viewport = viewport;
        }
        const auto& shared = session.descriptors();
        const auto shared_generation = shared.nativeBindingGeneration();
        const bool is_d3d12 = session.description().graphicsAPI == nri::GraphicsAPI::D3D12;
        const bool shared_changed = frame.shared_descriptors.generation != shared_generation;
        const bool resize_pool = shared_changed && frame.shared_descriptors.capacities != shared.snapshotCounts();
        // Freeze borrows the selected arrays only while materializing this Job's FIF.
        // The device round reuses the capacity across Jobs, including after a failed Freeze.
        draws.resize(plan.programs().rasterStreams().size());
        cxx::scope_exit release_lists([&]() noexcept { draws.clear(); });
        for (const auto& storage : cpu_draws_) draws[storage.compiledStream().list] = storage.view();
        for (const auto& source : draw_list_sources)
        {
            auto& input = draws[source.stream.list];
            input.data = frame.passEnabled(plan.passes()[source.stream.pass]) ? source.read(source.owner, slot) : RasterStreamData{};
            input.written = true;
        }
        if (resize_pool || !draws.empty())
        {
            if (auto capacity = frame.prepareDescriptorCapacity(gpu_, draws, resize_pool); !capacity)
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
            const auto& dependencies = plan.recordingDependencies();
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
        if (!draws.empty())
        {
            if (auto sections = frame.prepareSections(session, draws); !sections) return cxx::unexpected(sections.error());
            if (auto streams = frame.materializeStreams(gpu_, draws); !streams) return cxx::unexpected(streams.error());
        }
        auto frozen = frame.freeze(histories_, session.description());
        if (!frozen) return cxx::unexpected(frozen.error());
        return true;
    }

    void JobExecution::submitted(std::uint32_t frame_slot) noexcept
    {
        progress.last_submitted_slot = frame_slot;
        ++progress.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
        const auto& frame = frame_bindings.frames[frame_slot];
        for (auto& history : histories_)
        {
            const auto& sample = history.plan->description;
            const bool enabled = !sample.publish_when || conditionEnabled(*sample.publish_when, &frame);
            const bool produced = !sample.select_primary || conditionEnabled(*sample.select_primary, &frame);
            if (enabled && produced) history.publish();
        }
    }

    void JobExecution::acceptedBatch(std::uint32_t slot, std::uint32_t batch, SubmissionPoint point) noexcept
    {
        gpu_.accepted.accept(point);
        for (const auto& read : frame_bindings.frames[slot].targetReads(batch)) read.uses.accepted->accept(point);
        for (const auto& access : graph().submissions()[batch].resource_frontiers)
            if (auto* uses = frame_bindings.frames[slot].bindings[access.resource_index].queue_uses.accepted)
                uses->accept(point);
        if (batch != 0) return;
        const auto acceptClear = [&](JobHistoryResources::Slot& slot)
        {
            slot.last_use.accept(point);
            slot.initialized = true;
        };
        for (auto& history : histories_)
        {
            if (history.clear_previous) acceptClear(history.slots[history.previous]);
            if (history.clear_current) acceptClear(history.slots[history.current]);
        }
    }
} // namespace lux::render::runtime_detail
