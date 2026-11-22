#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/execute/RasterFramePreparation.hpp>
#include <algorithm>
#include <array>

namespace lux::render::runtime_detail
{
    using detail::FrameResources;
    using detail::JobHistoryResources;
    using detail::RasterDrawPacket;
    using detail::RasterFramePreparation;
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
            (!plan.compatible(output_.description) || output_.frames.size() != frames_.size());
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
            const auto& history = graph_.histories()[i];
            const auto distance = history.description.distance;
            if (frames_.size() > kGraphInvalidIndex - distance) return cxx::unexpected(RenderError{kDeviceCapacity});
            auto& bank = gpu_.histories[i];
            bank.slots.resize(frames_.size() + distance);
            const auto& resource = graph_.resources()[history.description.current.index];
            if (auto created = bank.initialize(session, history, resource, output_.description); !created) return created;
            histories_[i].bindBank(history, bank);
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            frames_[slot].initializeRasterStreams(plan.recordingDependencies());
            if (auto resources = createFrameResources(slot); !resources)
            {
                return resources;
            }
            auto* view = output_.frames.empty() ? nullptr : output_.frames[slot].view;
            if (auto passes = frames_[slot].prepareBindings(plan, session, view); !passes)
            {
                return passes;
            }
            frames_[slot].prepareAliasBarriers(plan.physicalResources());
            frames_[slot].command_batches.resize(plan.submissions().size());
            if (auto recordings = frames_[slot].native.createBatches(session, plan.submissions()); !recordings)
            {
                return recordings;
            }
        }
        auto pool = plan.bindingPlan().private_pool;
        if (auto counted = session.descriptors().addSnapshotDescriptors(pool); !counted) return counted;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            if (auto descriptors = frames_[slot].createDescriptors(plan, session, pool); !descriptors) return descriptors;
            frames_[slot].linkRecordInputs(plan);
            frames_[slot].bindHistory(histories_, plan.recordingDependencies());
            frames_[slot].updateHistoryViews(histories_, session.nriCore());
        }
        return {};
    }

    bool JobExecution::conditionEnabled(const FrameResources& frame, PassCondition condition) const noexcept
    {
        switch (condition.source)
        {
        case EConditionSource::HISTORY_VALID: return bool(frame.history_valid[condition.input]) == condition.expected;
        case EConditionSource::SAME_BINDING: return bool(binding_values_[condition.input]) == condition.expected;
        case EConditionSource::INPUT: return bool(frame.conditions[condition.input]) == condition.expected;
        }
        std::terminate();
    }

    RenderResult<void> JobExecution::invalidateHistory(std::span<const GraphResourceId> previous) noexcept
    {
        // Validate the whole request before changing any instance state.
        for (auto resource : previous)
        {
            const bool valid = resource.index < graph_.resources().size() &&
                               graph_.resources()[resource.index].origin == EGraphOrigin::HISTORY_PREVIOUS &&
                               graph_.resources()[resource.index].history != kGraphInvalidIndex;
            if (!valid)
            {
                return cxx::unexpected(RenderError{kGraphUnknownResource});
            }
        }
        if (previous.empty())
        {
            for (auto& history : histories_)
            {
                history.invalidate = true;
            }
        }
        else
        {
            for (auto resource : previous)
            {
                histories_[graph_.resources()[resource.index].history].invalidate = true;
            }
        }
        return {};
    }

    bool JobExecution::frozenCondition(std::uint32_t slot, PassCondition condition) const noexcept
    {
        return conditionEnabled(frames_[slot], condition);
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
        std::ranges::copy(values, condition_values_.begin());
        return {};
    }

    RenderResult<bool> JobExecution::freezeFrame(std::uint32_t slot) noexcept
    {
        auto& session = gpu_.session;
        const auto& plan = graph();
        if (const auto error = session.terminalError(); error.type) return cxx::unexpected(error);
        if (slot >= frames_.size()) return cxx::unexpected(RenderError{kDeviceArgument});
        frames_[slot].record_ready = false;
        if (submission_error_.type != 0)
        {
            return cxx::unexpected(submission_error_);
        }
        auto& frame = frames_[slot];
        if (plan.output() != kGraphInvalidIndex)
        {
            const auto& output = output_.frames[slot];
            auto& binding = frame.bindings[plan.output()];
            if (binding.image != output.image)
            {
                frame.retireRasterViews(session, nullptr, binding.image);
                frame.invalidateResource(plan.recordingDependencies(), plan.output());
                binding.select(nullptr, output.image, session.nriCore());
                // A presentation image has one writer and cannot be sampled by another instance.
                // Rotation changes this binding and its barriers, not the cross-instance dependency graph.
                if (output_.description.kind != ETargetKind::SURFACE)
                {
                    frame.invalidateImportedBindings();
                }
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
        std::ranges::copy(condition_values_, frame.conditions.begin());
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            const auto& sample = histories_[i].plan->description;
            const bool enabled = !sample.publish_when || conditionEnabled(frame, *sample.publish_when);
            const bool produced = !sample.select_primary || conditionEnabled(frame, *sample.select_primary);
            frame.publish_history[i] = enabled && produced;
        }
        auto changed_join = kGraphInvalidIndex;
        for (std::size_t r = 0; r < plan.recordingProgram().conditions.size(); ++r)
        {
            const auto& region = plan.recordingProgram().conditions[r];
            const bool enabled = conditionEnabled(frame, {region.source, region.input_slot, region.expected});
            const bool changed = (frame.enabled_regions[r] != 0) != enabled;
            if (changed)
            {
                if (enabled) frame.enabled_pass_count += region.pass_count;
                else frame.enabled_pass_count -= region.pass_count;
                for (auto p = region.first_pass; p < region.first_pass + region.pass_count; ++p)
                {
                    frame.invalidatePass(plan, p);
                    if (!enabled) continue;
                    const auto& offsets = plan.recordingDependencies().pass_field_offsets;
                    for (auto i = offsets[p]; i < offsets[p + 1]; ++i) frame.pending_operand_fields.mark(i);
                }
            }
            const auto join = region.binding_join;
            if (join != kGraphInvalidIndex)
            {
                const auto fallback = plan.recordingProgram().binding_joins[join].disabled_join;
                const bool inherited = !enabled && fallback != kGraphInvalidIndex && fallback == changed_join;
                if ((changed || inherited) && frame.selectBindingJoin(plan, join, enabled)) changed_join = join;
            }
            frame.enabled_regions[r] = enabled;
        }
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
        RasterFramePreparation raster(plan, frame, gpu_);
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
        if (!frame.stream_inputs.empty())
        {
            if (auto sections = raster.prepareSections(); !sections) return cxx::unexpected(sections.error());
            if (auto streams = raster.materializeStreams(); !streams) return cxx::unexpected(streams.error());
        }
        auto frozen = frame.freeze(histories_, session.description());
        if (!frozen) return cxx::unexpected(frozen.error());
        return true;
    }

    void JobExecution::submitted(std::uint32_t frame_slot) noexcept
    {
        progress.last_submitted_slot = frame_slot;
        ++progress.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
        if (histories_.empty())
        {
            return;
        }
        const auto& frame = frames_[frame_slot];
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            if (frame.publish_history[i])
            {
                histories_[i].publish();
            }
        }
    }

    void JobExecution::acceptedBatch(std::uint32_t slot, std::uint32_t batch, SubmissionPoint point) noexcept
    {
        gpu_.accepted.accept(point);
        for (auto id : graph().submissions()[batch].imported_resources)
            if (auto* uses = frames_[slot].selectedUses(id.index).accepted) uses->accept(point);
        const auto mark = [&](JobHistoryResources::Slot& slot)
        {
            slot.last_use.accept(point);
            slot.initialized = true;
        };
        if (batch == 0)
        {
            for (auto& history : histories_)
            {
                if (history.clear_previous)
                {
                    mark(history.slots[history.previous]);
                }
                if (history.clear_current)
                {
                    mark(history.slots[history.current]);
                }
            }
        }
        for (auto resource : graph().submissions()[batch].histories)
        {
            const auto& compiled = graph().resources()[resource.index];
            auto& history = histories_[compiled.history];
            mark(history.slots[compiled.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current]);
        }
    }
} // namespace lux::render::runtime_detail
