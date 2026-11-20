#include <algorithm>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>

namespace lux::render::detail
{
    RenderGraphInstance::RenderGraphInstance(
        DeviceSession& session,
        std::uint32_t frame_count,
        const RenderGraphExecutable& plan,
        const NativeGraphPrograms& programs,
        TargetOutput output
    ) noexcept
        : session_(&session), uses_(session.makeFrontiers()), plan_(&plan), programs_(&programs), output_(output)
    {
        condition_values_.resize(plan.condition_count_);
        const auto parameter_bytes = plan.passes().empty() ? 0 :
            plan.passes().back().parameter_offset + plan.passes().back().parameter_size;
        for (const auto& use : plan.binding_plan_->descriptor_uses)
        {
            if (use.sampler == kGraphInvalidIndex)
            {
                continue;
            }
            if (use.sampler >= samplers_.size())
            {
                samplers_.resize(std::size_t(use.sampler) + 1);
            }
            samplers_[use.sampler].comparison = use.comparison_sampler;
            samplers_[use.sampler].used = true;
        }
        frames_.resize(frame_count);
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
            frame.history_initializations.reserve(plan.histories_.size() * 2);
            frame.enabled_regions.resize(plan.recording_.conditions.size(), 1);
            frame.history_valid.resize(plan.histories_.size(), 0);
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
            frame.images.reserve(std::ranges::count_if(
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
        while (!producers_.empty()) producers_.pop_back();
        for (std::uint32_t pass = 0; pass < dynamic_raster_programs_.size(); ++pass)
            releaseDynamicRasterPrograms(pass, false);
        // Physical cleanup is owned by the Session; it never calls back into this instance.
        session_->deferRelease(
            uses_,
            [frames = std::move(frames_), histories = std::move(histories_), nri_core = &session_->nriCore()](
            ) mutable noexcept
            {
                for (auto& frame : frames)
                {
                    frame.release(*nri_core);
                }
                for (auto& history : histories)
                {
                    history.release(*nri_core);
                }
            }
        );
    }

    RenderResult<void> RenderGraphInstance::initialize() noexcept
    {
        auto& session = *session_;
        const auto& plan = *plan_;
        const bool wrong_device = plan.device() != &session.device();
        const bool wrong_target =
            !output_.frames.empty() &&
            (!plan.compatible(output_.description) || output_.frames.size() != frames_.size());
        const bool needs_target = plan.output() != kGraphInvalidIndex ||
            std::ranges::any_of(plan.resources(), [](const auto& resource) noexcept { return resource.target_sized; });
        const bool missing_target = needs_target && output_.frames.empty();
        const bool invalid_programs = programs_->session() != &session || programs_->size() != plan.passes().size();
        const bool missing_programs = invalid_programs || std::ranges::any_of(plan.passes(), [&](const auto& pass)
        {
            const auto& bindings = plan.binding_plan_->pass(pass.binding_slot);
            const auto& native = (*programs_)[pass.binding_slot];
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
            initializeRasterStreams(slot);
            if (auto resources = createFrameResources(slot); !resources)
            {
                return resources;
            }
            if (auto passes = prepareFrameBindings(slot); !passes)
            {
                return passes;
            }
            prepareAliasBarriers(frames_[slot]);
            if (auto recordings = createRecordings(slot); !recordings)
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
            bindHistory(slot);
            updateHistoryViews(slot);
        }
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
        if (plan_->output() != kGraphInvalidIndex)
        {
            const auto& output = output_.frames[slot];
            auto& binding = frame.bindings[plan_->output()];
            if (binding.image != output.image)
            {
                retireRasterViews(frame, nullptr, binding.image);
                binding.image = output.image;
                // A presentation image has one writer and cannot be sampled by another instance.
                // Rotation changes this binding and its barriers, not the cross-instance dependency graph.
                if (output_.description.kind != ETargetKind::SURFACE)
                {
                    frame.invalidateImportedBindings();
                }
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(plan_->output());
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
        if (!selectHistory())
        {
            return false;
        }
        bindHistory(slot);
        frame.enabled_pass_count = static_cast<std::uint32_t>(frame.passes.size());
        for (std::size_t r = 0; r < plan_->recording_.conditions.size(); ++r)
        {
            const auto& region = plan_->recording_.conditions[r];
            const bool value = region.source == EConditionSource::INPUT
                ? condition_values_[region.input_slot] != 0 : frame.history_valid[region.input_slot] != 0;
            const bool enabled = value == region.expected;
            if ((frame.enabled_regions[r] != 0) != enabled)
            {
                for (auto p = region.first_pass; p < region.first_pass + region.pass_count; ++p)
                    invalidateRecording(slot, p);
            }
            frame.enabled_regions[r] = enabled;
            if (!frame.enabled_regions[r])
            {
                frame.enabled_pass_count -= region.pass_count;
            }
        }
        if (!frame.changed_resources.empty())
        {
            frame.record_inputs_valid = false;
            if (auto rebound = rebind(slot); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        updateHistoryViews(slot);
        if (frame.viewport != viewport_)
        {
            for (std::uint32_t p = 0; p < frame.passes.size(); ++p)
            {
                const auto* raster = frame.passes[p].compiled_pass->rasterScope();
                if (raster && !raster->viewport) invalidateRecording(slot, p);
            }
            frame.viewport = viewport_;
        }
        const auto& shared = session_->descriptors();
        const auto shared_generation = shared.nativeBindingGeneration();
        const bool is_d3d12 = session_->description().graphicsAPI == nri::GraphicsAPI::D3D12;
        if (frame.shared_binding_generation != shared_generation)
        {
            if (frame.shared_descriptor_counts != shared.snapshotCounts())
            {
                if (auto grown = growRasterDescriptorPool(slot); !grown) return cxx::unexpected(grown.error());
            }
            if (!is_d3d12)
            {
                constexpr std::array kinds{
                    nri::DescriptorType::TEXTURE, nri::DescriptorType::SAMPLER, nri::DescriptorType::STRUCTURED_BUFFER
                };
                for (std::size_t kind = 0; kind < kinds.size(); ++kind)
                    frame.shared_descriptors[kind] = shared.descriptors(kinds[kind]);
                invalidateRecordings(slot);
            }
            frame.shared_binding_generation = shared_generation;
        }
        if (is_d3d12 && frame.shared_content_revision != shared.contentRevision())
        {
            shared.updateSnapshot(frame.shared_descriptors);
            frame.shared_content_revision = shared.contentRevision();
        }
        if (auto sections = prepareRasterSections(slot); !sections)
            return cxx::unexpected(sections.error());
        if (auto streams = materializeRasterStreams(slot); !streams)
            return cxx::unexpected(streams.error());
        prepareStreamedDraws(slot);
        const bool has_dynamic_record_input =
            !histories_.empty() || !frame.raster_draws.empty();
        if (!frame.record_inputs_valid || has_dynamic_record_input)
        {
            freezeRecordInput(slot);
            frame.record_inputs_valid = true;
        }
        frame.record_ready = true;
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
} // namespace lux::render::detail
