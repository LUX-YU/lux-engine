#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    namespace
    {
        bool coversInitialRanges(
            const detail::StorageGraphImport& owner, const detail::StorageGraphImport& required
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

        bool validStorageSchema(const detail::StorageGraphImport& schema) noexcept
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
            const EffectStorageImport& required, const detail::StorageGraphImport& actual
        ) noexcept
        {
            const auto& declared = std::get<detail::StorageGraphImport>(required.contract);
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

    RenderResult<std::vector<std::uint32_t>>
    RenderServer::Impl::bindEffectResources(
        detail::DeviceEntry& entry, ViewRequest job_scope,
        std::span<const EffectStorageImport> declarations,
        std::span<const JobResourceInput> inputs
    ) noexcept
    {
        auto& registry = runtime_->resources_;
        if (std::ranges::adjacent_find(inputs, {}, &JobResourceInput::binding) != inputs.end())
            return cxx::unexpected(RenderError{kCommandContract});
        if (inputs.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
        std::vector<std::uint32_t> resolved(declarations.size(), kGraphInvalidIndex);
        std::size_t matched = 0;
        for (std::uint32_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            if (input.binding.empty()) return cxx::unexpected(RenderError{kCommandContract, {i}});
            const auto found = std::ranges::lower_bound(
                declarations, input.binding, {}, &EffectStorageImport::binding
            );
            if (found == declarations.end() || found->binding != input.binding) continue;
            const auto& declaration = *found;
            const auto* factory = registry.factory(registry.declarationOf(input.resource));
            if (!factory) return cxx::unexpected(RenderError{kStorageIdentity, {i}});
            if (registry.deviceOf(input.resource) != &entry.session())
                return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            if (registry.hasExclusiveCommand(input.resource)) return cxx::unexpected(RenderError{kDeviceBusy, {i}});
            const auto* ops = factory->storage()->bindings();
            const bool has_gpu_ops = ops && ops->resolveGraphImport && ops->describeGraphImport;
            if (input.use == EJobResourceUse::GPU_BINDING && !has_gpu_ops)
                return cxx::unexpected(RenderError{kStorageContract, {i}});
            const bool wrong_owner = factory->descriptor->type.name() != declaration.type.name() ||
                input.projection != declaration.projection;
            if (wrong_owner) return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            if (input.publication && input.use != EJobResourceUse::GPU_BINDING)
                return cxx::unexpected(RenderError{kStorageContract, {i}});
            if (const auto* draw = std::get_if<EffectStorageImport::DrawContract>(&declaration.contract))
            {
                const auto type = factory->storage()->implementation_type;
                const bool wrong_type = type.hash() != draw->hash || type.name() != draw->name;
                if (input.use != EJobResourceUse::DRAW_RESOURCE || wrong_type)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                if ((draw->required_capabilities & ~entry.session().resourceCapabilities()) != 0)
                    return cxx::unexpected(RenderError{kDeviceUnsupported, {i}});
            }
            else if (auto* type = std::get_if<EffectStorageImport::SnapshotContract>(&declaration.contract))
            {
                const auto* ops_snapshot = ops ? ops->cpu_snapshot : nullptr;
                if (input.use != EJobResourceUse::CPU_SNAPSHOT || !ops_snapshot)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                if (!ops_snapshot->create || !ops_snapshot->capture)
                    return cxx::unexpected(RenderError{kStorageContract, {i}});
                if (type->scope == ESnapshotScope::JOB_LOCAL)
                {
                    const auto declaration_id = registry.declarationOf(input.resource);
                    auto owner = resourceScope(entry, job_scope, declaration_id, false);
                    if (!owner) return cxx::unexpected(owner.error());
                    auto local = (*owner)->query(entry.resourceCreateContext(), declaration_id);
                    if (!local) return cxx::unexpected(local.error());
                    if (*local != input.resource) return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                }
                if (ops_snapshot->type.hash() != type->hash || ops_snapshot->type.name() != type->name)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            }
            else
            {
                if (input.use != EJobResourceUse::GPU_BINDING)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                auto actual = ops->describeGraphImport(registry.resourceInstance(input.resource), input.projection);
                if (!actual) return cxx::unexpected(actual.error());
                if (!validStorageSchema(*actual)) return cxx::unexpected(RenderError{kStorageContract, {i}});
                if (input.publication)
                {
                    if (!actual->final_buffer_output) return cxx::unexpected(RenderError{kStorageContract, {i}});
                    auto selected = registry.selectProduct(input.resource, input.projection, *input.publication);
                    if (!selected) return cxx::unexpected(selected.error());
                    actual->description = GraphBufferDesc{selected->size};
                    actual->initial_ranges = {GraphBufferRange{0, selected->size}};
                    actual->final_buffer_output = false;
                }
                if (auto valid = qualifyGpuImport(declaration, *actual); !valid)
                    return cxx::unexpected(valid.error());
            }
            resolved[found - declarations.begin()] = i;
            ++matched;
        }
        for (std::size_t slot = 0; slot < resolved.size(); ++slot)
        {
            if (resolved[slot] == kGraphInvalidIndex && !declarations[slot].optional)
                return cxx::unexpected(RenderError{kStorageIdentity, {slot}});
        }
        if (matched != inputs.size()) return cxx::unexpected(RenderError{kCommandContract});
        return resolved;
    }
}
