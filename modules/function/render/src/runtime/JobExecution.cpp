#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/resource/GraphInstallation.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render::runtime_detail
{
    JobExecution::JobExecution(
        detail::JobGpuResources& gpu,
        const compiled::RenderGraph& selected,
        Installation installation,
        detail::TargetOutput physical
    ) noexcept
        : effect(installation.effect), device(installation.device), target(installation.target),
          graph(selected), executable(graph.executable),
          instance(gpu, executable, graph.programs, physical),
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
        const auto declared_targets = std::span{candidate.graph.target_imports};
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
            candidate.target_images.push_back({input.target, id});
            for (std::uint32_t slot = 0; slot < entry.frameProgress().frameCount(); ++slot)
            {
                candidate.instance.bindImage(id, slot, *images.frames[slot].image);
            }
        }
        if (target_images.size() < declared_targets.size()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (target_images.size() > declared_targets.size()) return cxx::unexpected(RenderError{kCommandContract});
        const auto& slots = candidate.graph.storage_imports;
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
        selected.reserve(candidate.graph.binding_comparisons.size());
        for (const auto& comparison : candidate.graph.binding_comparisons) selected.push_back(same_binding(comparison));
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
                id, input.projection, compiled.resource, compiled.output_range, compiled.canonical_queue
            };
            link.uses = std::span(candidate.graph.output_uses).subspan(compiled.first_use, compiled.use_count);
            const auto exports = std::span{candidate.graph.buffer_exports};
            link.exports = exports.subspan(compiled.first_export, compiled.export_count);
            link.publication = input.publication;
            if (!link.publication && std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                candidate.gpu_resources.push_back(id);
            const auto& recording = candidate.executable.recordingProgram();
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
        if (auto ready = candidate.instance.bindPredicates(std::move(selected)); !ready) return ready;
        if (auto ready = candidate.instance.initialize(); !ready) return ready;
        const auto& preparation = candidate.graph.preparation;
        detail::RenderGraphInstance::PreparationSources inputs;
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
                id, registry.resourceInstance(id), factory->storage()->implementation_type, input.capabilities
            };
            const auto* ops = factory->storage()->bindings();
            if (!ops || !ops->prepareFrame) continue;
            if (std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                candidate.gpu_resources.push_back(id);
        }
        if (auto ready = candidate.instance.createProducers(preparation, registry, std::move(inputs)); !ready)
            return ready;
        candidate.instance.linkUniformBlocks(candidate.graph.uniforms);
        if (auto values = candidate.instance.setConditions(input.conditions); !values) return values;
        candidate.instance.setViewport(input.viewport);
        return {};
    }
} // namespace lux::render
