#include <lux/engine/render/graph/Builder.hpp>

#include <bit>
#include <numeric>
#include <lux/engine/render/graph/detail/Range.hpp>

namespace lux::render
{
    using detail::graphError;

    namespace
    {
        RenderResult<void> validateResource(const GraphResource& resource) noexcept
        {
            const bool is_invalid_origin = resource.origin > EGraphOrigin::HISTORY_CURRENT;
            const bool has_illegal_initial = !resource.initial_ranges.empty() &&
                resource.origin != EGraphOrigin::IMPORTED && resource.origin != EGraphOrigin::HISTORY_PREVIOUS;
            if (resource.key.empty() || is_invalid_origin || has_illegal_initial)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            if (const auto* buffer = std::get_if<GraphBufferDesc>(&resource.description))
            {
                if (buffer->size == 0) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            }
            else
            {
                const auto& image = std::get<GraphImageDesc>(resource.description);
                const bool is_invalid_extent = image.width == 0 || image.height == 0 || image.layers == 0;
                const bool is_invalid_mips = image.mips == 0 ||
                    image.mips > std::bit_width(std::max(image.width, image.height));
                const bool is_invalid_samples = !std::has_single_bit(image.samples) || image.samples > 64 ||
                    (image.samples > 1 && image.mips != 1);
                const bool is_invalid_format = rdesc::textureAspectMask(image.format) == 0;
                if (is_invalid_extent || is_invalid_mips || is_invalid_samples || is_invalid_format)
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            }
            return {};
        }

        template <typename T>
        RenderResult<void> uniqueKeys(std::span<const T> values, bool passes) noexcept
        {
            std::vector<std::uint32_t> order(values.size());
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&](auto a, auto b) { return values[a].key < values[b].key; });
            for (std::size_t i = 1; i < order.size(); ++i)
            {
                if (values[order[i - 1]].key == values[order[i]].key)
                {
                    return cxx::unexpected(graphError(kGraphDuplicateKey,
                        {passes ? order[i] : kGraphInvalidIndex},
                        {passes ? kGraphInvalidIndex : order[i]}, order[i - 1]));
                }
            }
            return {};
        }
    }

    RenderResult<GraphResourceId> RenderGraphBuilder::declareResource(GraphResource resource) noexcept
    {
        const GraphResourceId id{static_cast<std::uint32_t>(candidate_.resources_.size())};
        if (candidate_.resources_.size() >= kGraphInvalidIndex)
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        if (auto valid = validateResource(resource); !valid)
            return cxx::unexpected(graphError(valid.error().type, {}, id));
        for (auto& range : resource.initial_ranges)
        {
            auto normalized = detail::normalize(resource, range);
            if (!normalized) return cxx::unexpected(graphError(kGraphInvalidRange, {}, id));
            range = *normalized;
        }
        candidate_.resources_.push_back(std::move(resource));
        return id;
    }

    RenderResult<GraphPassId> RenderGraphBuilder::addPass(GraphPass pass) noexcept
    {
        const GraphPassId id{static_cast<std::uint32_t>(candidate_.passes_.size())};
        const bool is_invalid_pass = candidate_.passes_.size() >= kGraphInvalidIndex || pass.key.empty() ||
            pass.kind > EGraphPassKind::TRANSFER;
        if (is_invalid_pass) return cxx::unexpected(graphError(kGraphInvalidDescriptor, id));
        for (auto& use : pass.uses)
        {
            const auto resource_id = use.version.resource;
            if (resource_id.index >= candidate_.resources_.size())
                return cxx::unexpected(graphError(kGraphUnknownResource, id, resource_id));
            const auto& resource = candidate_.resources_[resource_id.index];
            const bool is_write = detail::writes(use.access);
            const bool is_invalid_access = use.access > EGraphAccess::ATTACHMENT_LOAD ||
                (is_write && (use.version.number == 0 || resource.origin == EGraphOrigin::HISTORY_PREVIOUS));
            const bool is_invalid_predecessor = is_write && detail::reads(use.access) &&
                use.input_version >= use.version.number;
            const auto* image = std::get_if<GraphImageDesc>(&resource.description);
            const bool is_attachment_format = image &&
                (rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::COLOR_ATTACHMENT) ||
                 rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT));
            const bool is_invalid_load = use.access == EGraphAccess::ATTACHMENT_LOAD &&
                (pass.kind != EGraphPassKind::GRAPHICS || !is_attachment_format);
            if (is_invalid_access || is_invalid_predecessor || is_invalid_load)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            // BOUNDARY: validate the orthogonal execution role here, once at authoring.
            const bool is_invalid_color = use.usage == EGraphUse::COLOR_ATTACHMENT &&
                (!image || pass.kind != EGraphPassKind::GRAPHICS ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::COLOR_ATTACHMENT) ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::ATTACHMENT_LOAD));
            const bool is_invalid_source = use.usage == EGraphUse::TRANSFER_SOURCE &&
                (pass.kind != EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ);
            const bool is_invalid_destination = use.usage == EGraphUse::TRANSFER_DESTINATION &&
                (pass.kind != EGraphPassKind::TRANSFER || use.access != EGraphAccess::WRITE);
            const bool is_invalid_sample = use.usage == EGraphUse::SAMPLED &&
                (!image || pass.kind == EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::SAMPLED));
            const bool is_storage = use.usage == EGraphUse::STORAGE_READ ||
                use.usage == EGraphUse::STORAGE_WRITE || use.usage == EGraphUse::STORAGE_READ_WRITE;
            const bool invalid_storage = is_storage && (image || pass.kind == EGraphPassKind::TRANSFER ||
                (use.usage == EGraphUse::STORAGE_WRITE && use.access != EGraphAccess::WRITE) ||
                (use.usage == EGraphUse::STORAGE_READ_WRITE && use.access != EGraphAccess::READ_WRITE));
            const bool geometry = use.usage == EGraphUse::VERTEX || use.usage == EGraphUse::INDEX ||
                use.usage == EGraphUse::INDIRECT;
            const bool invalid_geometry = geometry &&
                (image || pass.kind != EGraphPassKind::GRAPHICS || use.access != EGraphAccess::READ);
            const bool is_invalid_usage = use.usage > EGraphUse::INDIRECT || invalid_storage || invalid_geometry ||
                (use.usage == EGraphUse::STORAGE_READ && (image || use.access != EGraphAccess::READ)) ||
                is_invalid_color || is_invalid_source || is_invalid_destination || is_invalid_sample;
            if (is_invalid_usage) return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            auto range = detail::normalize(resource, use.range);
            if (!range) return cxx::unexpected(graphError(kGraphInvalidRange, id, resource_id));
            use.range = *range;
            if (use.fallback)
            {
                const auto source = use.fallback->resource;
                if (source.index >= candidate_.resources_.size())
                    return cxx::unexpected(graphError(kGraphUnknownResource, id, source));
                const bool is_compatible = candidate_.resources_[source.index].description == resource.description;
                const bool is_write_fallback = is_write && pass.conditional && source == resource_id &&
                    use.fallback->number < use.version.number;
                const bool is_history_fallback = !is_write && resource.origin == EGraphOrigin::HISTORY_PREVIOUS &&
                    use.version.number == 0 && source != resource_id;
                if (!is_compatible || (!is_write_fallback && !is_history_fallback))
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            }
        }
        // One pass must express an overlapping in-place update as READ_WRITE/LOAD,
        // not two independent accesses with unprovable internal ordering.
        for (std::size_t i = 0; i < pass.uses.size(); ++i)
        {
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto& a = pass.uses[i];
                const auto& b = pass.uses[j];
                const bool has_write = detail::writes(a.access) || detail::writes(b.access);
                const bool is_conflict = a.version.resource == b.version.resource && has_write &&
                    detail::overlaps(a.range, b.range);
                if (is_conflict) return cxx::unexpected(graphError(kGraphConflictingOutput, id, a.version.resource, j));
            }
        }
        candidate_.passes_.push_back(std::move(pass));
        return id;
    }

    RenderResult<void> RenderGraphBuilder::dependsOn(GraphPassId before, GraphPassId after) noexcept
    {
        const bool is_unknown = before.index >= candidate_.passes_.size() || after.index >= candidate_.passes_.size();
        if (is_unknown) return cxx::unexpected(graphError(kGraphUnknownPass, before, {}, after.index));
        if (before == after) return cxx::unexpected(graphError(kGraphDependencyCycle, before));
        const GraphDependency edge{before, after};
        auto& edges = candidate_.dependencies_;
        if (std::find(edges.begin(), edges.end(), edge) == edges.end()) edges.push_back(edge);
        return {};
    }

    RenderResult<void> RenderGraphBuilder::exportOutput(GraphOutput output) noexcept
    {
        const auto id = output.version.resource;
        if (id.index >= candidate_.resources_.size())
            return cxx::unexpected(graphError(kGraphUnknownResource, {}, id));
        const auto& resource = candidate_.resources_[id.index];
        auto range = detail::normalize(resource, output.range);
        if (!range) return cxx::unexpected(graphError(kGraphInvalidRange, {}, id));
        output.range = *range;
        if (output.target)
        {
            if (auto valid = output.target->validate(); !valid) return cxx::unexpected(valid.error());
            const auto* image = std::get_if<GraphImageDesc>(&resource.description);
            const auto* part = std::get_if<GraphImageRange>(&output.range);
            const auto& target = *output.target;
            const bool is_compatible = image && part &&
                image->width == target.width && image->height == target.height &&
                image->format == target.format && image->samples == target.samples && part->aspects == 1 &&
                part->first_mip == 0 && part->mip_count == 1 && part->layer_count == 1;
            if (!is_compatible) return cxx::unexpected(graphError(kGraphConflictingOutput, {}, id));
        }
        for (const auto& existing : candidate_.outputs_)
        {
            const bool is_conflict = existing.version.resource == id && detail::overlaps(existing.range, output.range);
            if (is_conflict) return cxx::unexpected(graphError(kGraphConflictingOutput, {}, id));
        }
        candidate_.outputs_.push_back(std::move(output));
        return {};
    }

    RenderResult<RenderGraphDefinition> RenderGraphBuilder::finish() && noexcept
    {
        if (auto valid = uniqueKeys(candidate_.resources(), false); !valid) return cxx::unexpected(valid.error());
        if (auto valid = uniqueKeys(candidate_.passes(), true); !valid) return cxx::unexpected(valid.error());
        auto result = std::move(candidate_);
        candidate_ = {};
        return result;
    }

    std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept
    {
        using error::EArgument;
        static constexpr auto args = std::array{EArgument::UNSIGNED, EArgument::UNSIGNED, EArgument::UNSIGNED};
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.graph.unknown_resource", "Unknown owner-local resource", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.unknown_pass", "Unknown owner-local pass", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.invalid_descriptor", "Invalid graph declaration", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.invalid_range", "Invalid resource range", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.duplicate_key", "Duplicate complete key", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.conflicting_output", "Conflicting output", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.undefined_read", "Version range has no producer", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.ambiguous_producer", "Overlapping version producers", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.dependency_cycle", "Dependency cycle", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.missing_fallback", "Conditional fallback missing", error::ERecovery::PERMANENT, args},
            {"lux.render.graph.missing_initial", "Initial range unavailable", error::ERecovery::PERMANENT, args}
        };
        return descriptors;
    }
}
