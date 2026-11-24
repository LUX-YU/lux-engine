#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render::runtime_detail
{
    detail::InstalledFrameResource ResolvedResource::frameSource() const noexcept
    {
        return {id, instance, factory->storage().bindings()};
    }
}

namespace lux::render
{
    namespace
    {
        bool coversInitialRanges(
            const ResourceProjection& owner, const ResourceProjection& required
        ) noexcept
        {
            if (owner.initial_ranges == required.initial_ranges) return true;
            const GraphResource provided{"", owner.description};
            const GraphResource requested{"", required.description};
            std::vector<detail::Box> remaining, next;
            for (const auto& range : required.initial_ranges)
            {
                const auto normalized = detail::normalize(requested, range);
                if (!normalized) return false;
                for (const auto& box : detail::boxes(*normalized)) remaining.push_back(box);
            }
            for (const auto& range : owner.initial_ranges)
            {
                const auto normalized = detail::normalize(provided, range);
                if (!normalized) return false;
                for (const auto& box : detail::boxes(*normalized))
                {
                    next.clear();
                    for (const auto& piece : remaining) detail::subtract(piece, box, next);
                    remaining.swap(next);
                }
            }
            return remaining.empty();
        }

        bool validStorageSchema(const ResourceProjection& schema) noexcept
        {
            const bool buffer = std::holds_alternative<GraphBufferDesc>(schema.description);
            const bool invalid_writer = schema.final_buffer_output && (schema.read_use || !buffer);
            const bool read_layout = schema.initial_use == EGraphUse::SAMPLED ||
                schema.initial_use == EGraphUse::TRANSFER_SOURCE;
            const bool invalid_concurrency = schema.concurrent_image_read &&
                (buffer || schema.read_use != schema.initial_use || !read_layout);
            return !invalid_writer && !invalid_concurrency && schema.initial_use != EGraphUse::UNSPECIFIED;
        }

        RenderResult<void> qualifyGpuImport(
            const EffectResourceManifest::GpuImport& required, const ResourceProjection& actual
        ) noexcept
        {
            const auto& declared = required.contract;
            const bool missing_output = declared.final_buffer_output && !actual.final_buffer_output;
            const bool missing_concurrency = declared.concurrent_image_read &&
                (!actual.concurrent_image_read || actual.read_use != declared.read_use);
            if (missing_output || missing_concurrency || declared.initial_use != actual.initial_use)
                return cxx::unexpected(RenderError{kStorageMismatch});
            const auto* requested = std::get_if<GraphBufferDesc>(&declared.description);
            const auto* available = std::get_if<GraphBufferDesc>(&actual.description);
            const bool wrong_shape = requested ? !available || requested->size > available->size :
                declared.description != actual.description;
            if (wrong_shape || !coversInitialRanges(actual, declared))
                return cxx::unexpected(RenderError{kStorageMismatch});
            if (actual.read_use)
            {
                const auto use = static_cast<unsigned>(*actual.read_use);
                if (use > static_cast<unsigned>(EGraphUse::UNIFORM_READ))
                    return cxx::unexpected(RenderError{kStorageContract});
                if (required.graph_uses & ~(1u << use)) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            return {};
        }
    }

    RenderServer::Impl::ResolvedResources
    RenderServer::Impl::installScopedInputs(
        JobExecution& execution, ResourceRegistry::BindingChanges& installation, ViewScope scope
    ) noexcept
    {
        const auto& graph = execution.graph();
        const auto& manifest = execution.device_graph->resources.requirements;
        if (manifest.empty()) return std::vector<ResolvedResource>{};
        auto& device = *runtime_->device(execution.device);
        std::vector<ResolvedResource> instances;
        instances.reserve(manifest.size());
        for (const auto& requirement : manifest)
        {
            auto context = device.resourceCreateContext();
            auto owner = requirement.factory->descriptor->domain == EResourceDomain::JOB ?
                jobResourceScope(device, execution, scope, &context) : resourceScope(device, scope, requirement.type, &context);
            if (!owner) return cxx::unexpected(owner.error());
            context.created_ = &installation.created;
            auto instance = (*owner)->getOrCreate(context, requirement.type, requirement.key);
            storages_created_.store(runtime_->resources_.creations(), std::memory_order_relaxed);
            if (!instance) return cxx::unexpected(instance.error());
            auto used = runtime_->resources_.useForBinding(installation, *instance, requirement.type, device);
            if (!used) return cxx::unexpected(used.error());
            instances.emplace_back(*instance, *used, requirement.factory);
        }
        const auto frameSource = [&](std::uint32_t slot) noexcept
        {
            auto binding = instances[slot].frameSource();
            execution.useFrameResource(binding);
            return binding;
        };
        const auto& resources = execution.device_graph->resources;
        for (const auto& binding : resources.raster_sources)
        {
            const auto& stream = graph.programs().rasterStreams()[binding.stream];
            if (binding.programs != kGraphInvalidIndex)
            {
                const auto source = frameSource(binding.programs);
                const auto& publication = source.ops->rasterPrograms(source.instance);
                if (auto bound = execution.bindPublication(stream, publication); !bound)
                    return cxx::unexpected(bound.error());
            }
            if (binding.draw_list != kGraphInvalidIndex)
            {
                const auto source = frameSource(binding.draw_list);
                execution.draw_list_sources.push_back({stream, source.instance, source.ops->readDrawList});
            }
        }
        for (const auto field : resources.buffer_extents)
        {
            const auto source = frameSource(field.requirement);
            const auto bytes = source.ops->bindBufferExtent(source.instance, field.field);
            if (bytes.size() != execution.frameCount())
                return cxx::unexpected(RenderError{kStorageContract, {field.requirement, field.field}});
            execution.buffer_extents.push_back(bytes);
        }
        return instances;
    }

    RenderServer::Impl::ResolvedResources RenderServer::Impl::bindEffectResources(
        JobExecution& execution, ResourceRegistry::BindingChanges& installation, ViewScope job_scope,
        std::span<const JobResourceInput> inputs
    ) noexcept
    {
        if (inputs.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (std::ranges::adjacent_find(inputs, {}, &JobResourceInput::binding) != inputs.end())
            return cxx::unexpected(RenderError{kCommandContract});
        auto defaults = installScopedInputs(execution, installation, job_scope);
        if (!defaults) return cxx::unexpected(defaults.error());
        auto& entry = *runtime_->device(execution.device);
        const auto& manifest = execution.device_graph->resources;
        const auto& declarations = manifest.operands;
        auto& registry = runtime_->resources_;
        std::vector<ResolvedResource> resolved(declarations.size());
        std::size_t next_input = 0;
        for (std::size_t slot = 0; slot < declarations.size(); ++slot)
        {
            const auto& declaration = declarations[slot];
            const auto* supplied = next_input < inputs.size() ? &inputs[next_input] : nullptr;
            if (supplied && supplied->binding < declaration.binding) return cxx::unexpected(RenderError{kCommandContract});
            if (supplied && supplied->binding != declaration.binding) supplied = nullptr;
            const auto i = supplied ? next_input++ : slot;
            auto& input = resolved[slot];
            if (declaration.requirement != kGraphInvalidIndex)
            {
                input = (*defaults)[declaration.requirement];
                if (supplied && supplied->resource != input.id) return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            }
            else if (supplied)
            {
                const auto id = supplied->resource;
                auto used = registry.useForBinding(installation, id, declaration.type, entry);
                if (!used)
                {
                    const auto error = used.error();
                    if (error.type == kDeviceBusy) return cxx::unexpected(error);
                    return cxx::unexpected(RenderError{error.type, {i}});
                }
                input = {id, *used, registry.factory(declaration.type)};
            }
            else if (declaration.optional) continue;
            else return cxx::unexpected(RenderError{kStorageIdentity, {i}});

            const auto* factory = input.factory;
            if (supplied && supplied->projection != declaration.projection)
                return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            if (declaration.binding_error) return cxx::unexpected(RenderError{declaration.binding_error, {i}});
            if (supplied && supplied->publication) input.publication = &*supplied->publication;
            if (input.publication && declaration.gpu_import == kGraphInvalidIndex)
                return cxx::unexpected(RenderError{kStorageContract, {i}});
            bool job_local = false;
            if (declaration.gpu_import != kGraphInvalidIndex)
            {
                const auto& gpu = manifest.gpu_imports[declaration.gpu_import];
                const auto* ops = factory->storage().bindings();
                auto actual = ops->describeGraphImport(input.instance, declaration.projection);
                if (!actual) return cxx::unexpected(actual.error());
                job_local = actual->job_local || gpu.contract.job_local;
                if (!validStorageSchema(*actual)) return cxx::unexpected(RenderError{kStorageContract, {i}});
                if (input.publication)
                {
                    if (!actual->final_buffer_output) return cxx::unexpected(RenderError{kStorageContract, {i}});
                    auto selected = registry.selectProduct(input.id, declaration.projection, *input.publication);
                    if (!selected) return cxx::unexpected(selected.error());
                    actual->description = GraphBufferDesc{selected->size};
                    actual->initial_ranges = {GraphBufferRange{0, selected->size}};
                    actual->final_buffer_output = false;
                }
                if (auto valid = qualifyGpuImport(gpu, *actual); !valid)
                    return cxx::unexpected(valid.error());
            }
            if (declaration.requirement != kGraphInvalidIndex) continue;
            if (factory->descriptor->domain == EResourceDomain::JOB)
            {
                const auto* owner = execution.privateResources();
                if (!owner || !owner->contains(input.id)) return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            }
            else if (job_local)
            {
                auto owner = resourceScope(entry, job_scope, declaration.type, nullptr);
                if (!owner) return cxx::unexpected(owner.error());
                if (!(*owner)->contains(input.id)) return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            }
        }
        if (next_input != inputs.size()) return cxx::unexpected(RenderError{kCommandContract});
        return resolved;
    }
}
