#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>

namespace lux::render::runtime_detail
{
    JobExecution::JobExecution(
        detail::DeviceEntry& entry,
        const detail::EffectDeviceVariant& selected,
        TargetId output,
        detail::TargetOutput physical
    ) noexcept
        : effect(selected.effect), device(entry.id), variant(selected.id), target(output),
          graph(*selected.plan), executable(graph.executable),
          instance(entry.session(), entry.frameProgress().frameCount(), executable, graph.programs, physical)
    {
    }

    void JobExecution::selectViewParameters() noexcept
    {
        if (!view_parameters || view_revision == view_parameters->revision()) return;
        view_revision = view_parameters->revision();
        instance.selectViewParameters(view_parameters->parameters(), view_revision);
    }

    JobExecution::~JobExecution() noexcept
    {
        if (pending_uses || programs.stop)
        {
            std::terminate();
        }
    }

} // namespace lux::render::runtime_detail

namespace lux::render
{
    RenderResult<void>
    RenderServer::Impl::assembleExecution(
        JobExecution& candidate, const JobBinding& binding, const RenderEffect& effect
    ) noexcept
    {
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
        if (slots.size() != binding.resources.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        const auto same_binding = [&](const compiled::BindingComparison& comparison) noexcept
        {
            const auto [left, right] = comparison;
            if (left.domain != right.domain) return false;
            if (left.domain == compiled::BindingOperand::EDomain::TARGET)
                return target_images[left.slot].target == target_images[right.slot].target;
            const auto& a = binding.resources[left.slot];
            const auto& b = binding.resources[right.slot];
            return a.resource == b.resource && a.projection == b.projection && a.use == b.use;
        };
        std::vector<std::uint8_t> selected;
        selected.reserve(candidate.graph.binding_comparisons.size());
        for (const auto& comparison : candidate.graph.binding_comparisons) selected.push_back(same_binding(comparison));
        candidate.storage_bindings.reserve(slots.size());
        auto& registry = runtime_->resources_;
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto slot = slots[i].resource.index;
            if (slot == kGraphInvalidIndex) continue;
            const auto& input = binding.resources[i];
            const auto id = input.resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            if (std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                candidate.gpu_resources.push_back(id);
            BoundStorageProjection link{
                id, input.projection, slots[i].resource, slots[i].output_range, slots[i].canonical_queue
            };
            const auto uses = std::span(candidate.graph.output_passes).subspan(slots[i].first_use, slots[i].use_count);
            const auto& recording = candidate.executable.recordingProgram();
            const bool may_access = !link.output_range || std::ranges::any_of(uses, [&](auto pass)
            {
                const auto region = recording.passes[pass].condition_region;
                if (region == kGraphInvalidIndex) return true;
                const auto& guard = recording.conditions[region];
                return guard.source != EConditionSource::SAME_BINDING ||
                    bool(selected[guard.input_slot]) == guard.expected;
            });
            auto bound = registry.bindGraphProjection(link, candidate.storage_start, candidate.instance, may_access);
            if (!bound) return cxx::unexpected(bound.error());
            link.subscription = *bound;
            candidate.storage_bindings.push_back(link);
        }
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
            const auto id = binding.resources[snapshot.binding].resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            const auto* factory = registry.factory(registry.declarationOf(id));
            const auto* ops = factory->storage()->bindings();
            sources.push_back({id, registry.resourceInstance(id), ops->cpu_snapshot});
        }
        const auto bindOwner = [&]<class Storage>(std::uint32_t slot, Storage*& owner) noexcept
            -> RenderResult<void>
        {
            if (slot == kGraphInvalidIndex) return {};
            const auto id = binding.resources[slot].resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            auto storage = registry.borrow<Storage>(id);
            if (!storage) return cxx::unexpected(storage.error());
            owner = *storage;
            return {};
        };
        auto& raster_owners = inputs.raster_owners;
        raster_owners.reserve(preparation.raster_bindings.size());
        for (const auto& bindings : preparation.raster_bindings)
        {
            auto& owners = raster_owners.emplace_back();
            if (auto ready = bindOwner(bindings.textures, owners.textures); !ready) return ready;
            if (auto ready = bindOwner(bindings.meshes, owners.meshes); !ready) return ready;
            if (auto ready = bindOwner(bindings.materials, owners.materials); !ready) return ready;
            if (owners.materials)
            {
                const auto id = owners.materials->id();
                if (std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                    candidate.gpu_resources.push_back(id);
            }
        }
        if (auto ready = candidate.instance.createProducers(preparation, registry, std::move(inputs)); !ready)
            return ready;
        candidate.instance.linkUniformBlocks(candidate.graph.uniforms);
        if (auto values = candidate.instance.setConditions(input.conditions); !values) return values;
        candidate.instance.setViewport(input.viewport);
        candidate.selectViewParameters();
        return {};
    }
} // namespace lux::render
