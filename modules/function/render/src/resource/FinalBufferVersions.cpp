#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>

namespace lux::render
{
    using runtime_detail::BoundStorageProjection;

    ResourceRegistry::ProjectionBuffer ResourceRegistry::ProjectionBuffer::part(GraphBufferRange range) const noexcept
    {
        if (!buffer || !detail::contains({0, bytes.size}, range)) return {};
        return {buffer, {bytes.offset + range.offset, range.size}};
    }

    bool ResourceRegistry::ProjectionBuffer::overlaps(ProjectionBuffer other) const noexcept
    {
        if (!buffer || buffer != other.buffer || !bytes.size || !other.bytes.size) return false;
        return bytes.offset <= other.bytes.offset ? other.bytes.offset - bytes.offset < bytes.size :
            bytes.offset - other.bytes.offset < other.bytes.size;
    }

    void ResourceRegistry::reserveProductBuffer(ResourceInstanceId id, nri::Buffer* buffer) noexcept
    {
        if (!buffer) return;
        auto& buffers = entries_.find(id)->product_buffers;
        if (std::ranges::find(buffers, buffer, &ProductBufferUse::buffer) != buffers.end()) return;
        const auto* device = deviceOf(id);
        buffers.push_back({buffer, device->makeFrontiers(), device->makeFrontiers()});
    }

    RenderResult<void> ResourceRegistry::replaceProductBacking(
        ResourceInstanceId id, std::uint32_t projection, std::uint32_t first, std::span<const ProjectionBuffer> slices
    ) noexcept
    {
        auto& entry = *entries_.find(id);
        auto& products = entry.products;
        if (entry.product_buffers.empty()) return {};
        const auto* device = deviceOf(id);
        const auto replaceable = [&](nri::Buffer* buffer)
        {
            const auto use = std::ranges::find(entry.product_buffers, buffer, &ProductBufferUse::buffer);
            return use == entry.product_buffers.end() || (!use->prepared_readers && !use->prepared_writers &&
                device->reached(use->reads) && device->reached(use->writes));
        };
        const auto binding = entry.graph_bindings.find(projection);
        const bool has_writer = binding != entry.graph_bindings.end() &&
            std::ranges::any_of(binding->second.users, [](const auto& user) { return user.writer.has_value(); });
        if (has_writer)
            for (std::size_t i = 0; i < slices.size(); ++i)
            {
                const auto& old = binding->second.backing.buffers[first + i];
                if (old != slices[i] && !replaceable(old.buffer)) return cxx::unexpected(RenderError{kDeviceBusy});
            }
        for (const auto& product : products)
        {
            if (product.projection != projection) continue;
            for (std::size_t i = 0; i < slices.size(); ++i)
            {
                const auto& sample = product.samples[first + i];
                if (slices[i].part(product.range) == sample.backing) continue;
                if (!replaceable(sample.backing.buffer)) return cxx::unexpected(RenderError{kDeviceBusy});
            }
        }
        for (auto& product : products)
        {
            if (product.projection != projection) continue;
            for (std::size_t i = 0; i < slices.size(); ++i)
            {
                auto& sample = product.samples[first + i];
                const auto next = slices[i].part(product.range);
                if (next == sample.backing) continue;
                sample.backing = next;
                sample.writer = {};
            }
        }
        if (has_writer)
            for (const auto& slice : slices) reserveProductBuffer(id, slice.buffer);
        return {};
    }

    void ResourceRegistry::pruneBufferProducts(ResourceInstanceId id) noexcept
    {
        auto& entry = *entries_.find(id);
        std::erase_if(entry.products, [&](const auto& product)
        {
            const bool published = std::ranges::any_of(product.samples, [](const auto& sample)
            {
                return !sample.writer.isNull();
            });
            if (published) return false;
            const auto projection = entry.graph_bindings.find(product.projection);
            return projection == entry.graph_bindings.end() ||
                std::ranges::none_of(projection->second.users, [&](const auto& user)
                {
                    return user.writer && detail::contains(*user.writer, product.range);
                });
        });
        const auto* device = deviceOf(id);
        std::erase_if(entry.product_buffers, [&](const auto& use)
        {
            const bool prepared = use.prepared_readers || use.prepared_writers;
            if (prepared || !device->reached(use.reads) || !device->reached(use.writes)) return false;
            for (const auto& product : entry.products)
                for (const auto& sample : product.samples)
                    if (sample.backing.buffer == use.buffer) return false;
            for (const auto& [projection, bindings] : entry.graph_bindings)
                for (const auto& slice : bindings.backing.buffers)
                    if (slice.buffer == use.buffer) return false;
            return true;
        });
    }

    RenderResult<detail::ResourceInput>
    ResourceRegistry::selectProduct(
        ResourceInstanceId id, std::uint32_t projection, FinalBufferVersion version
    ) const noexcept
    {
        const auto* entry = entries_.find(id);
        if (!entry || version.writer.isNull() || version.slot_index >= 3)
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (!version.range.size) return cxx::unexpected(RenderError{kGraphInvalidRange});
        bool has_revision = false;
        for (const auto& product : entry->products)
        {
            if (product.projection != projection) continue;
            const auto& sample = product.samples[version.slot_index];
            if (sample.writer != version.writer || sample.frame_index != version.frame_index) continue;
            has_revision = true;
            if (!detail::contains(product.range, version.range)) continue;
            const auto offset = sample.backing.bytes.offset + (version.range.offset - product.range.offset);
            return detail::ResourceInput{sample.backing.buffer, offset, version.range.size};
        }
        return cxx::unexpected(RenderError{has_revision ? kGraphInvalidRange : kRuntimeIdentity});
    }

    RenderResult<bool> ResourceRegistry::pinBufferUse(const BoundStorageProjection& link, std::uint32_t slot) noexcept
    {
        auto& entry = *entries_.find(link.storage);
        const auto* device = deviceOf(link.storage);
        if (link.publication)
        {
            auto selected = selectProduct(link.storage, link.projection, *link.publication);
            if (!selected) return cxx::unexpected(selected.error());
            auto& use = *std::ranges::find(entry.product_buffers, selected->buffer, &ProductBufferUse::buffer);
            if (use.prepared_writers || !device->reached(use.writes)) return false;
            const bool shared_state = link.canonical_queue != kGraphInvalidIndex &&
                link.canonical_queue == use.read_queue && link.projection == use.read_projection;
            if (!shared_state && (use.prepared_readers || !device->reached(use.reads))) return false;
            if (use.prepared_readers == UINT32_MAX) std::terminate();
            ++use.prepared_readers;
            use.read_queue = link.canonical_queue;
            use.read_projection = link.projection;
        }
        else if (link.output_range)
        {
            const auto buffer = entry.graph_bindings.find(link.projection)->second.backing.buffers[slot].buffer;
            auto& use = *std::ranges::find(entry.product_buffers, buffer, &ProductBufferUse::buffer);
            if (use.prepared_readers || !device->reached(use.reads) || !device->reached(use.writes)) return false;
            if (use.prepared_writers == UINT32_MAX) std::terminate();
            ++use.prepared_writers;
        }
        return true;
    }

    void ResourceRegistry::unpinBufferUse(const BoundStorageProjection& link, std::uint32_t slot) noexcept
    {
        auto& entry = *entries_.find(link.storage);
        if (link.publication)
        {
            const auto selected = selectProduct(link.storage, link.projection, *link.publication);
            if (!selected) std::terminate();
            auto& use = *std::ranges::find(entry.product_buffers, selected->buffer, &ProductBufferUse::buffer);
            if (!use.prepared_readers) std::terminate();
            --use.prepared_readers;
        }
        else if (link.output_range)
        {
            const auto buffer = entry.graph_bindings.find(link.projection)->second.backing.buffers[slot].buffer;
            auto& use = *std::ranges::find(entry.product_buffers, buffer, &ProductBufferUse::buffer);
            if (!use.prepared_writers) std::terminate();
            --use.prepared_writers;
        }
    }

    RenderResult<bool>
    ResourceRegistry::pinBufferUses(std::span<const BoundStorageProjection> links, std::uint32_t slot) noexcept
    {
        for (std::size_t i = 0; i < links.size(); ++i)
        {
            const auto& link = links[i];
            if (!link.output_range && !link.publication) continue;
            auto pinned = pinBufferUse(link, slot);
            if (pinned && *pinned) continue;
            unpinBufferUses(links.first(i), slot);
            if (!pinned) return cxx::unexpected(pinned.error());
            // An overlap with another operand of this very execution cannot be resolved by waiting.
            auto alone = pinBufferUse(link, slot);
            if (alone && *alone)
            {
                unpinBufferUse(link, slot);
                return cxx::unexpected(RenderError{kGraphConflictingOutput});
            }
            return false;
        }
        return true;
    }

    void ResourceRegistry::unpinBufferUses(std::span<const BoundStorageProjection> links, std::uint32_t slot) noexcept
    {
        for (const auto& link : links)
            if (link.output_range || link.publication) unpinBufferUse(link, slot);
    }

    void ResourceRegistry::acceptProductUse(
        const BoundStorageProjection& link, std::uint32_t slot, const detail::SubmissionFrontiers& uses
    ) noexcept
    {
        auto& entry = *entries_.find(link.storage);
        if (link.publication)
        {
            const auto selected = selectProduct(link.storage, link.projection, *link.publication);
            if (!selected) std::terminate();
            std::ranges::find(entry.product_buffers, selected->buffer, &ProductBufferUse::buffer)->reads.merge(uses);
            return;
        }
        const auto buffer = entry.graph_bindings.find(link.projection)->second.backing.buffers[slot].buffer;
        std::ranges::find(entry.product_buffers, buffer, &ProductBufferUse::buffer)->writes.merge(uses);
    }

    void ResourceRegistry::invalidateProduct(
        const BoundStorageProjection& link, std::uint32_t slot, GraphBufferRange range
    ) noexcept
    {
        auto& entry = *entries_.find(link.storage);
        const auto dest = entry.graph_bindings.find(link.projection)->second.backing.buffers[slot].part(range);
        for (auto& product : entry.products)
            for (auto& sample : product.samples)
                if (dest.overlaps(sample.backing)) sample.writer = {};
    }

    void ResourceRegistry::publishProduct(const BoundStorageProjection& link, FinalBufferVersion version) noexcept
    {
        auto& products = entries_.find(link.storage)->products;
        for (auto& product : products)
        {
            if (product.projection != link.projection || product.range != version.range) continue;
            auto& sample = product.samples[version.slot_index];
            sample.writer = version.writer;
            sample.frame_index = version.frame_index;
            return;
        }
        std::terminate();
    }
}
