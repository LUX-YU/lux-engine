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
          graph_plan(selected.plan.get()), executable(graph_plan->executable),
          instance(entry.session(), entry.frameProgress().frameCount(), executable, graph_plan->programs, physical)
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

    RenderResult<void> JobExecution::bindStorage(
        std::uint32_t resource,
        const detail::StorageBindingOps& ops,
        void* native,
        std::uint32_t frame_count,
        std::uint32_t projection
    ) noexcept
    {
        auto& use = storage_inputs.emplace_back(&instance, resource);
        use.subscription.receiver = &use;
        use.subscription.changed = [](void* value, std::uint32_t slot, const detail::ResourceInput& resource) noexcept
        {
            auto& link = *static_cast<JobExecution::StorageInput*>(value);
            link.instance->setResource(link.resource, slot, resource);
        };
        std::array<detail::ResourceInput, 3> slices;
        const auto& description = executable.resources()[resource].description;
        auto output = std::span(slices).first(frame_count);
        auto resolved = ops.resolveGraphImport(native, projection, description, use.subscription, output);
        if (!resolved)
        {
            return cxx::unexpected(resolved.error());
        }
        for (std::uint32_t slot = 0; slot < frame_count; ++slot)
        {
            instance.setResource(resource, slot, slices[slot]);
        }
        return {};
    }

} // namespace lux::render::runtime_detail

namespace lux::render
{
    RenderResult<void>
    RenderServer::Impl::bindGraphSamplers(JobExecution& candidate, std::span<const SamplerDescription> samplers) noexcept
    {
        auto& pool = runtime_->device(candidate.device)->session().descriptors();
        auto& instance = candidate.instance;
        for (std::uint32_t slot = 0; slot < instance.samplerInputCount(); ++slot)
        {
            if (!instance.usesSampler(slot)) continue;
            if (slot >= samplers.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            auto sampler = pool.sampler(samplers[slot]);
            if (!sampler) return cxx::unexpected(sampler.error());
            if (auto bound = instance.bindSampler(slot, **sampler, samplers[slot].comparison.has_value()); !bound)
                return bound;
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::assembleExecution(JobExecution& candidate, const JobBinding& binding) noexcept
    {
        const auto& plan = candidate.executable;
        const auto target_images = std::span(binding.target_images);
        const auto& input = binding.inputs;
        auto& entry = *runtime_->device(candidate.device);
        candidate.target_images.reserve(target_images.size());
        if (candidate.graph_plan->target_imports.size() != target_images.size())
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        for (std::size_t index = 0; index < target_images.size(); ++index)
        {
            const auto& input = target_images[index];
            const auto id = candidate.graph_plan->target_imports[index];
            if (id.index == kGraphInvalidIndex) continue;
            const auto* source = runtime_->targets_.find(input.target);
            if (!source || !targetDescription(*source).sampled)
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto& resource = plan.resources()[id.index];
            const auto* image = std::get_if<GraphImageDesc>(&resource.description);
            const bool duplicate = std::ranges::any_of(
                candidate.target_images,
                [id](const auto& imported) noexcept { return imported.resource == id; }
            );
            const bool invalid_import = resource.origin != EGraphOrigin::IMPORTED || !resource.sampled_target_input;
            if (duplicate || !image || !resource.memory_used || invalid_import)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            // Static imported-resource structure. This belongs in Graph compilation
            // once the compiled import contract is available to Attachment creation.
            if (image->layers != 1 || image->mips != 1)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if (targetState(*source).resizing)
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }

            const auto images = targetOutput(*source);
            // Dynamic properties can change when a Target is resized. Target::description
            // is already validated as sampled above, so do not test it a second time.
            if (images.description.samples != image->samples || images.description.format != image->format)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }

            candidate.target_images.push_back({input.target, id});
            for (std::uint32_t slot = 0; slot < entry.frameProgress().frameCount(); ++slot)
            {
                candidate.instance.bindImage(id, slot, *images.frames[slot].image);
            }
        }
        const auto& slots = candidate.graph_plan->storage_imports;
        if (slots.size() != binding.resources.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        candidate.storage_inputs.reserve(slots.size());
        auto& registry = runtime_->resources_;
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto slot = slots[i].index;
            if (slot == kGraphInvalidIndex) continue;
            const auto& input = binding.resources[i];
            const auto id = input.resource;
            if (auto retained = retainExecutionResource(candidate, id); !retained) return retained;
            if (std::ranges::find(candidate.gpu_resources, id) == candidate.gpu_resources.end())
                candidate.gpu_resources.push_back(id);
            const auto* factory = registry.factory(registry.declarationOf(id));
            const auto& ops = *factory->storage()->bindings();
            if (auto bound = candidate.bindStorage(
                    slot, ops, registry.resourceInstance(id), entry.frameProgress().frameCount(),
                    input.projection
                ); !bound) return bound;
        }
        const auto samplers = std::span{candidate.graph_plan->samplers};
        if (auto bound = bindGraphSamplers(candidate, samplers); !bound) return bound;
        if (auto ready = candidate.instance.initialize(); !ready) return ready;
        const auto& preparation = candidate.graph_plan->preparation;
        std::vector<detail::RenderGraphInstance::SnapshotSource> sources;
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
        std::vector<detail::RenderGraphInstance::RasterOwners> raster_owners;
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
        if (auto ready = candidate.instance.createProducers(
                preparation, registry, sources, std::move(raster_owners)
            ); !ready) return ready;
        candidate.instance.linkUniformBlocks(candidate.graph_plan->uniforms);
        if (auto values = candidate.instance.setConditions(input.conditions); !values) return values;
        candidate.instance.setViewport(input.viewport);
        candidate.selectViewParameters();
        return {};
    }
} // namespace lux::render
