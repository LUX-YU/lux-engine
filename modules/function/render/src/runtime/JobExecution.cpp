#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <lux/engine/render/features/common/ParameterLayout.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>

namespace lux::render::runtime_detail
{
    JobExecution::RecipeUse::~RecipeUse() noexcept
    {
        effect.releaseExecution();
    }

    JobExecution::JobExecution(
        RenderEffect& effect,
        DeviceSessionId session,
        detail::DeviceSession& device,
        std::uint32_t frame_count,
        detail::RenderGraphExecutable&& plan,
        detail::NativeGraphPrograms&& native_programs,
        TargetId output,
        detail::TargetOutput physical
    ) noexcept
        : recipe{effect}, device(session), target(output), executable(std::move(plan)),
          fixed_programs(std::move(native_programs)), instance(device, frame_count, executable, fixed_programs, physical)
    {
    }

    JobExecution::~JobExecution() noexcept
    {
        if (pending_uses)
        {
            std::terminate();
        }
    }

    RenderResult<void> JobExecution::bindStorage(
        std::uint32_t resource,
        const detail::StorageBindingOps& ops,
        void* native,
        const void* argument,
        std::uint32_t frame_count
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
        auto resolved = ops.resolve(
            native,
            executable.resources()[resource].description,
            argument,
            use.subscription,
            std::span(slices).first(frame_count)
        );
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
    RenderResult<std::vector<ResourceInstanceId>>
    RenderServer::Impl::resolveJobResources(const RenderEffect& effect, detail::DeviceEntry& device, ViewRequest scope) noexcept
    {
        auto context = device.resourceCreateContext();
        std::vector<ResourceInstanceId> dependency_storages;
        dependency_storages.reserve(effect.resource_dependencies.size());
        for (const auto& dependency : effect.resource_dependencies)
        {
            auto* owner = dependency.scope == EResourceDomain::DEVICE ? &device.resources() :
                device.scopedResources(scope.scene, dependency.scope == EResourceDomain::VIEW ? scope.view : ViewId{}, true);
            auto instance = owner->getOrCreate(context, dependency.declaration);
            if (!instance)
            {
                return cxx::unexpected(instance.error());
            }
            dependency_storages.push_back(*instance);
        }
        return dependency_storages;
    }

    RenderResult<void> RenderServer::Impl::assembleExecution(
        const detail::GraphAssemblyPlan& assembly,
        JobExecution& candidate,
        RenderView& view,
        std::span<const ResourceInstanceId> dependency_storages,
        const JobBinding& binding
    ) noexcept
    {
        const auto& effect = candidate.recipe.effect;
        const auto& plan = candidate.executable;
        const auto target_images = std::span(binding.target_images);
        const auto& input = binding.inputs;
        auto& entry = *runtime_->device(candidate.device);
        candidate.resources.assign(dependency_storages.begin(), dependency_storages.end());
        for (std::size_t i = 0; i < dependency_storages.size(); ++i)
        {
            const auto& dependency = effect.resource_dependencies[i];
            if (dependency.columns.empty())
            {
                continue;
            }
            const auto id = dependency_storages[i];
            const auto& call = runtime_->resources_.factory(dependency.declaration)->frame_call;
            candidate.inputs.emplace_back(id, runtime_->resources_.resourceInstance(id), &call, dependency.columns);
        }
        candidate.target_images.reserve(target_images.size());
        for (const auto& input : target_images)
        {
            const auto* source = runtime_->targets_.find(input.target);
            if (!source || !targetDescription(*source).sampled)
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto bound =
                std::ranges::find(effect.external_images, input.destination, &RenderEffect::ExternalImage::destination);
            if (bound == effect.external_images.end())
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto id = bound->resource;
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
        candidate.storage_inputs.reserve(plan.resources().size());
        std::vector<ResourceInstanceId> resolved(plan.resources().size());
        for (std::size_t dependency_index = 0; dependency_index < effect.resource_dependencies.size();
             ++dependency_index)
        {
            const auto& dependency = effect.resource_dependencies[dependency_index];
            const auto* factory = runtime_->resources_.factory(dependency.declaration);
            const auto id = dependency_storages[dependency_index];
            for (const auto& argument : dependency.bindings)
            {
                const auto index = argument.resource.index;
                const auto type = argument.type();
                if (type != factory->descriptor->binding_type)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto* value = argument.data();
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops);
                resolved[index] = id;
                if (auto bound =
                        candidate
                            .bindStorage(index, *ops, runtime_->resources_.resourceInstance(id), value, entry.frameProgress().frameCount());
                    !bound)
                {
                    return cxx::unexpected(bound.error());
                }
            }
        }
        const auto created = candidate.instance.createData(
            assembly, effect.graph.instanceData(), runtime_->resources_, resolved
        );
        if (!created)
            return cxx::unexpected(created.error());
        for (std::uint32_t input = 0; input < candidate.instance.samplerInputCount(); ++input)
        {
            if (!candidate.instance.usesSampler(input))
            {
                continue;
            }
            auto sampler = entry.session().descriptors().sampler(effect.graph.samplers()[input]);
            if (!sampler)
            {
                return cxx::unexpected(sampler.error());
            }
            if (auto bound =
                    candidate.instance.bindSampler(input, **sampler, effect.graph.samplers()[input].comparison.has_value());
                !bound)
            {
                return cxx::unexpected(bound.error());
            }
        }
        if (auto ready = candidate.instance.initialize(assembly); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        auto& instance = candidate.instance;
        auto view_source = instance.bindParameterSource(
            assembly, EParameterSource::VIEW, detail::cameraParameterLayout()
        );
        if (!view_source)
        {
            return cxx::unexpected(view_source.error());
        }
        auto effect_source = instance.bindParameterSource(
            assembly, EParameterSource::EFFECT, detail::effectParameterLayout()
        );
        if (!effect_source)
        {
            return cxx::unexpected(effect_source.error());
        }
        if (auto values = instance.setInputs(input.parameters, input.conditions); !values)
            return cxx::unexpected(values.error());
        instance.setViewport(input.viewport);
        detail::CameraParameters camera;
        if (auto encoded = detail::projectCameraParameters(view.camera, std::as_writable_bytes(std::span{&camera, 1}));
            !encoded)
        {
            return cxx::unexpected(encoded.error());
        }
        if (auto applied = instance.setParameterSource(EParameterSource::VIEW, std::as_bytes(std::span{&camera, 1}));
            !applied)
        {
            return cxx::unexpected(applied.error());
        }
        if (instance.usesParameterSource(EParameterSource::VIEW))
        {
            const auto& viewport = view.camera.viewport;
            instance.setViewport({viewport[0], viewport[1], viewport[2], viewport[3]});
        }
        candidate.camera_revision = view.camera_revision;
        return {};
    }
} // namespace lux::render
