#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/resource/GraphInstallation.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render::runtime_detail
{
    namespace
    {
        std::vector<detail::FrameResources> createFrames(std::span<detail::JobFrameResources> physical, const compiled::RenderGraph& graph) noexcept
        {
            std::vector<detail::FrameResources> frames;
            frames.reserve(physical.size());
            for (auto& frame : physical) frames.emplace_back(frame).initializeRows(graph);
            return frames;
        }
    }

    void JobExecution::settlePrograms(detail::RasterProgramUpdate& update) noexcept
    {
        programs.settle(update, frames_);
    }

    JobExecution::~JobExecution() noexcept
    {
        for (const auto& frame : frames_)
            for (const auto& batch : frame.command_batches)
                if (batch.recording.load(std::memory_order_acquire)) std::terminate();
    }

    JobExecution::JobExecution(
        detail::JobGpuResources& gpu,
        const compiled::RenderGraph& selected,
        Installation installation,
        detail::TargetOutput physical
    ) noexcept
        : effect(installation.effect), device(installation.device), target(installation.target),
          graph_(selected), output_(physical), gpu_(gpu), condition_values_(selected.conditionCount()),
          frames_(createFrames(gpu.frames, selected)), histories_(selected.histories().size()),
          programs(selected, gpu), preparation(selected, gpu, programs),
          binding_identity(installation.identity)
    {
    }

} // namespace lux::render::runtime_detail

namespace lux::render
{
    RenderResult<void>
    RenderServer::Impl::assembleExecution(
        ResourceRegistry::GraphInstallation& installation, const JobBinding& binding, const RenderEffect& effect,
        std::span<const std::uint32_t> columns
    ) noexcept
    {
        auto& candidate = installation.execution();
        const auto target_images = std::span(binding.target_images);
        const auto& input = binding.inputs;
        auto& entry = *runtime_->device(candidate.device);
        candidate.target_images.reserve(target_images.size());
        const auto declared_targets = std::span{candidate.graph().targetImports()};
        if (std::ranges::adjacent_find(target_images, {}, &TargetImageInput::binding) != target_images.end())
            return cxx::unexpected(RenderError{kCommandContract});
        const bool matching_count = target_images.size() == declared_targets.size();
        for (std::size_t index = 0; index < target_images.size(); ++index)
        {
            const auto& input = target_images[index];
            if (input.binding.empty()) return cxx::unexpected(RenderError{kCommandContract});
            const auto* source = runtime_->targets_.find(input.target);
            if (!source || !targetDescription(*source).sampled) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!std::holds_alternative<detail::OffscreenTarget>(*source))
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (targetState(*source).device != candidate.device) return cxx::unexpected(RenderError{kJobState});
            if (targetState(*source).resizing) return cxx::unexpected(RenderError{kDeviceBusy});
            if (!matching_count) continue;
            const auto& declaration = declared_targets[index];
            if (declaration.binding != input.binding) return cxx::unexpected(RenderError{kRuntimeIdentity, {index}});
            const auto images = targetOutput(*source);
            const bool matches = images.description.format == declaration.format &&
                images.description.samples == declaration.samples;
            if (!matches) return cxx::unexpected(RenderError{kDeviceArgument, {index}});
            const auto id = declaration.resource;
            if (id.index == kGraphInvalidIndex) continue;
            candidate.target_images.push_back({input.target, static_cast<std::uint32_t>(index)});
            for (std::uint32_t slot = 0; slot < entry.frameProgress().frameCount(); ++slot)
            {
                candidate.bindImage(id, slot, *images.frames[slot].image);
            }
        }
        if (target_images.size() < declared_targets.size()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (target_images.size() > declared_targets.size()) return cxx::unexpected(RenderError{kCommandContract});
        const auto& slots = candidate.graph().storageImports();
        if (slots.size() != columns.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        const auto same_binding = [&](const compiled::BindingComparison& comparison) noexcept
        {
            const auto [left, right] = comparison;
            if (left.domain != right.domain) return false;
            if (left.domain == compiled::BindingOperand::EDomain::TARGET)
                return target_images[left.slot].target == target_images[right.slot].target;
            const auto& a = binding.resources[columns[left.slot]];
            const auto& b = binding.resources[columns[right.slot]];
            return a.resource == b.resource && a.projection == b.projection && a.use == b.use &&
                a.publication == b.publication;
        };
        std::vector<std::uint8_t> selected;
        selected.reserve(candidate.graph().bindingComparisons().size());
        for (const auto& comparison : candidate.graph().bindingComparisons()) selected.push_back(same_binding(comparison));
        candidate.storage_bindings.reserve(slots.size());
        auto& registry = runtime_->resources_;
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto& compiled = slots[i].slot;
            const auto slot = compiled.resource.index;
            if (slot == kGraphInvalidIndex) continue;
            const auto& input = binding.resources[columns[i]];
            const auto id = input.resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            BoundStorageProjection link{
                id, input.projection, compiled.resource, compiled.output_range, compiled.boundary
            };
            link.uses = std::span(candidate.graph().outputUses()).subspan(compiled.first_use, compiled.use_count);
            const auto exports = std::span{candidate.graph().bufferExports()};
            link.exports = exports.subspan(compiled.first_export, compiled.export_count);
            link.publication = input.publication;
            if (!link.publication && std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                candidate.gpu_resources.push_back(id);
            const auto& recording = candidate.graph().recordingProgram();
            const bool may_access = !link.output_range || std::ranges::any_of(link.uses, [&](const auto& use)
            {
                const auto region = recording.passes[use.pass].condition_region;
                if (region == kGraphInvalidIndex) return true;
                const auto& guard = recording.conditions[region];
                return guard.source != EConditionSource::SAME_BINDING ||
                    bool(selected[guard.input_slot]) == guard.expected;
            });
            if (auto bound = installation.resolve(link, may_access); !bound) return bound;
            candidate.storage_bindings.push_back(link);
        }
        const auto ordinary = std::ranges::partition(candidate.storage_bindings, [](const auto& link)
        {
            return link.output_range || link.publication;
        });
        candidate.product_count = static_cast<std::size_t>(ordinary.begin() - candidate.storage_bindings.begin());
        if (auto ready = candidate.bindPredicates(std::move(selected)); !ready) return ready;
        if (auto ready = candidate.initialize(); !ready) return ready;
        const auto& preparation = candidate.graph().cpuProgram();
        detail::JobCpuPreparation::PreparationSources inputs;
        const auto buffers = entry.effectResources(candidate.effect);
        if (buffers.size() != preparation.constant_buffers.size()) std::terminate();
        inputs.constant_buffers.reserve(buffers.size());
        for (std::size_t i = 0; i < buffers.size(); ++i)
        {
            const auto& input = preparation.constant_buffers[i];
            const auto value = input.bind(effect.node_states[input.state].value.get());
            if (value.bytes.size() != input.bytes || !value.revision || !*value.revision)
                return cxx::unexpected(RenderError{kFrameContract, {i}});
            inputs.constant_buffers.push_back({input.resource, buffers[i].get(), value});
        }
        inputs.effect_values.reserve(preparation.effect_inputs.size());
        for (auto input : preparation.effect_inputs)
        {
            const auto& state = effect.node_states[input.source];
            const auto bytes = input.bind ? input.bind(state.value.get()) :
                std::span{static_cast<const std::byte*>(state.value.get()), state.bytes};
            inputs.effect_values.push_back(bytes);
        }
        auto& sources = inputs.snapshots;
        sources.reserve(preparation.snapshots.size());
        for (const auto& snapshot : preparation.snapshots)
        {
            const auto id = binding.resources[columns[snapshot.binding]].resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            const auto* factory = registry.factory(registry.declarationOf(id));
            const auto* ops = factory->storage()->bindings();
            sources.push_back({id, registry.resourceInstance(id), ops->cpu_snapshot});
        }
        inputs.draw_resources.reserve(preparation.draw_resource_bindings.size());
        for (const auto& input : preparation.draw_resource_bindings)
        {
            auto& source = inputs.draw_resources.emplace_back();
            const auto column = columns[input.binding];
            if (column == kGraphInvalidIndex) continue;
            const auto id = binding.resources[column].resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            const auto* factory = registry.factory(registry.declarationOf(id));
            source = {
                id, registry.resourceInstance(id), factory->storage()->implementation_type,
                input.capabilities, input.indexed_geometry
            };
            const auto* ops = factory->storage()->bindings();
            if (!ops || !ops->prepareFrame) continue;
            if (std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                candidate.gpu_resources.push_back(id);
        }
        if (auto linked = candidate.programs.bind(registry, inputs.snapshots, inputs.draw_resources); !linked)
            return linked;
        if (auto ready = candidate.preparation.bind(registry, std::move(inputs), candidate.targetDescription()); !ready)
            return ready;
        if (auto values = candidate.setConditions(input.conditions); !values) return values;
        candidate.viewport = input.viewport;
        return {};
    }
} // namespace lux::render
