#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/resource/InstalledProjection.hpp>

namespace lux::render
{
    using runtime_detail::BufferProductBinding;

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
        if (buffers.contains(buffer)) return;
        const auto* device = deviceOf(id);
        buffers.emplace(buffer, ProductBufferUse{{device->makeFrontiers()}, {device->makeFrontiers()}});
    }

    bool ResourceRegistry::ProjectionBindings::hasWriter() const noexcept
    {
        return std::ranges::any_of(users, [](const auto& user) { return user.slot->output_range.has_value(); });
    }

    RenderResult<void> ResourceRegistry::qualifyProductReplacement(
        ResourceInstanceId id, const GraphBufferPublication& update, std::uint32_t frame_count
    ) noexcept
    {
        const auto& entry = *entries_.find(id);
        if (entry.product_buffers.empty()) return {};
        const auto* device = deviceOf(id);
        const auto replaceable = [&](nri::Buffer* buffer)
        {
            const auto found = entry.product_buffers.find(buffer);
            if (found == entry.product_buffers.end()) return true;
            const auto& use = found->second;
            return !use.reads.prepared && !use.writes.prepared &&
                device->reached(use.reads.accepted) && device->reached(use.writes.accepted);
        };
        const ProjectionBuffer next{update.value.buffer, {update.value.offset, update.value.size}};
        const auto binding = entry.graph_bindings.find(update.projection);
        const bool has_writer = binding != entry.graph_bindings.end() && binding->second.hasWriter();
        for (std::uint32_t i = 0; i < frame_count; ++i)
        {
            if (update.frame != UINT32_MAX && update.frame != i) continue;
            if (has_writer)
            {
                const auto& old = binding->second.backing.buffers[i];
                if (old != next && !replaceable(old.buffer)) return cxx::unexpected(RenderError{kDeviceBusy});
            }
            for (const auto& product : entry.products)
            {
                if (product.projection != update.projection) continue;
                const auto& sample = product.samples[i];
                if (next.part(product.range) != sample.backing && !replaceable(sample.backing.buffer))
                    return cxx::unexpected(RenderError{kDeviceBusy});
            }
        }
        return {};
    }

    void ResourceRegistry::adoptProductReplacement(
        ResourceInstanceId id, const GraphBufferPublication& update, std::uint32_t frame_count
    ) noexcept
    {
        auto& entry = *entries_.find(id);
        if (entry.product_buffers.empty()) return;
        const ProjectionBuffer replacement{update.value.buffer, {update.value.offset, update.value.size}};
        for (auto& product : entry.products)
        {
            if (product.projection != update.projection) continue;
            const auto next = replacement.part(product.range);
            for (std::uint32_t i = 0; i < frame_count; ++i)
            {
                if (update.frame != UINT32_MAX && update.frame != i) continue;
                auto& sample = product.samples[i];
                if (next == sample.backing) continue;
                sample.backing = next;
                sample.writer = {};
            }
        }
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
                    const auto& written = user.slot->output_range;
                    return written && detail::contains(*written, product.range);
                });
        });
        const auto* device = deviceOf(id);
        std::erase_if(entry.product_buffers, [&](const auto& row)
        {
            const auto& [buffer, use] = row;
            if (use.reads.prepared || use.writes.prepared) return false;
            if (!device->reached(use.reads.accepted) || !device->reached(use.writes.accepted)) return false;
            for (const auto& product : entry.products)
                for (const auto& sample : product.samples)
                    if (sample.backing.buffer == buffer) return false;
            for (const auto& [projection, bindings] : entry.graph_bindings)
                for (const auto& slice : bindings.backing.buffers)
                    if (slice.buffer == buffer) return false;
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

    RenderResult<bool> ResourceRegistry::pinBufferUse(BufferProductBinding& link, std::uint32_t slot) noexcept
    {
        if (link.pin) std::terminate();
        auto& entry = *entries_.find(link.storage);
        nri::Buffer* buffer = nullptr;
        if (link.publication)
        {
            const auto selected = selectProduct(link.storage, link.projection, *link.publication);
            if (!selected) return cxx::unexpected(selected.error());
            buffer = selected->buffer;
        }
        else
        {
            buffer = entry.graph_bindings.find(link.projection)->second.backing.buffers[slot].buffer;
        }

        const auto found = entry.product_buffers.find(buffer);
        if (found == entry.product_buffers.end()) std::terminate();
        auto& use = found->second;
        const auto& device = *deviceOf(link.storage);
        const bool shared_state = link.slot->boundary.canShare(use.selected);
        if (link.publication)
        {
            // A prepared writer can still overwrite the publication. Once settled, its accepted
            // queue points are GPU dependencies of the reader's first use on each physical queue.
            if (use.writes.prepared) return false;
            if (!shared_state && (use.reads.prepared || !device.reached(use.reads.accepted))) return false;
            link.pin = &use.reads;
        }
        else
        {
            const bool gpu_busy = !device.reached(use.reads.accepted) || !device.reached(use.writes.accepted);
            if (use.reads.prepared || gpu_busy) return false;
            if (use.writes.prepared && !shared_state) return false;
            link.pin = &use.writes;
        }
        if (link.pin->prepared == UINT32_MAX) std::terminate();
        ++link.pin->prepared;
        use.selected = link.slot->boundary;
        return true;
    }

    void runtime_detail::BufferProductBinding::accept(detail::SubmissionPoint point) const noexcept
    {
        if (!pin) std::terminate();
        pin->accepted.accept(point);
    }

    void runtime_detail::BufferProductBinding::releasePin() noexcept
    {
        auto* use = std::exchange(pin, nullptr);
        if (!use || !use->prepared) std::terminate();
        --use->prepared;
    }

    RenderResult<bool>
    ResourceRegistry::pinBufferUses(std::span<BufferProductBinding> links, std::uint32_t slot) noexcept
    {
        for (std::size_t i = 0; i < links.size(); ++i)
        {
            auto& link = links[i];
            auto pinned = pinBufferUse(link, slot);
            if (pinned && *pinned) continue;
            unpinBufferUses(links.first(i));
            if (!pinned) return cxx::unexpected(pinned.error());
            // An overlap with another operand of this very execution cannot be resolved by waiting.
            auto alone = pinBufferUse(link, slot);
            if (alone && *alone)
            {
                link.releasePin();
                return cxx::unexpected(RenderError{kGraphConflictingOutput});
            }
            return false;
        }
        return true;
    }

    void ResourceRegistry::unpinBufferUses(std::span<BufferProductBinding> links) noexcept
    {
        for (auto& link : links) link.releasePin();
    }

    void ResourceRegistry::invalidateProduct(
        const BufferProductBinding& link, std::uint32_t slot, GraphBufferRange range
    ) noexcept
    {
        auto& entry = *entries_.find(link.storage);
        const auto dest = entry.graph_bindings.find(link.projection)->second.backing.buffers[slot].part(range);
        for (auto& product : entry.products)
            for (auto& sample : product.samples)
                if (dest.overlaps(sample.backing)) sample.writer = {};
    }

    void ResourceRegistry::publishProduct(const BufferProductBinding& link, FinalBufferVersion version) noexcept
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
