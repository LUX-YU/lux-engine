#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/InstalledProjection.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <algorithm>
#include <iterator>

namespace lux::render
{
    void ResourceRegistry::beginBindings(BindingChanges& changes) noexcept
    {
        if (pending_bindings_) std::terminate();
        pending_bindings_ = &changes;
    }

    void ResourceRegistry::endBindings(BindingChanges& changes) noexcept
    {
        if (pending_bindings_ != &changes) std::terminate();
        while (!changes.created.empty())
        {
            const auto id = changes.created.back();
            changes.created.pop_back();
            auto* entry = entries_.find(id);
            if (!entry || !entry->storage_set->remove(id)) std::terminate();
        }
        pending_bindings_ = nullptr;
    }

    bool ResourceRegistry::BindingChanges::holds(ResourceInstanceId id) const noexcept
    {
        return std::ranges::find(rows, id, &Resource::storage) != rows.end();
    }

    bool ResourceRegistry::BindingChanges::protects(ResourceInstanceId id) const noexcept
    {
        return std::ranges::find(resources, id) != resources.end() || std::ranges::find(created, id) != created.end();
    }

    RenderResult<void*> ResourceRegistry::useForBinding(
        BindingChanges& changes, ResourceInstanceId id, ResourceTypeId type, const detail::DeviceEntry& device
    ) noexcept
    {
        const auto* entry = entries_.find(id);
        if (!entry || !entry->instance || entry->storage_set->closed_) return cxx::unexpected(RenderError{kStorageIdentity});
        if (&entry->storage_set->device_ != &device) return cxx::unexpected(RenderError{kStorageMismatch});
        if (entry->exclusive_command) return cxx::unexpected(RenderError{kDeviceBusy});
        if (entry->type->id != type) return cxx::unexpected(RenderError{kStorageMismatch});
        if (std::ranges::find(changes.resources, id) == changes.resources.end()) changes.resources.push_back(id);
        return entry->instance;
    }

    RenderResult<void> ResourceRegistry::stageProjection(
        BindingChanges& pending, runtime_detail::ProjectionSubscription& link, const GraphBindingUse& user,
        const FinalBufferVersion* publication
    ) noexcept
    {
        if (std::ranges::find(pending.resources, link.storage) == pending.resources.end())
            return cxx::unexpected(RenderError{kStorageIdentity});
        // Claim the projection before the external resolver can reenter backing publication.
        const auto row = std::ranges::find(pending.rows, link.storage, &BindingChanges::Resource::storage);
        auto& changes = row == pending.rows.end() ? pending.rows.emplace_back(BindingChanges::Resource{link.storage}) : *row;
        if (publication)
        {
            auto value = selectProduct(link.storage, link.projection, *publication);
            if (!value) return cxx::unexpected(value.error());
            // Admission revalidates this exact publication and pins its physical Buffer before
            // submission borrows the frontier. A disjoint write can advance it after installation.
            const auto& buffers = entries_.find(link.storage)->product_buffers;
            const auto use = buffers.find(value->buffer);
            if (use == buffers.end()) std::terminate();
            value->queue_uses.preparation = &use->second.writes.accepted;
            user.target->bind(user.slot->resource, UINT32_MAX, *value);
            return {};
        }
        auto backing = resolveBacking(link, user);
        if (!backing) return cxx::unexpected(backing.error());
        if (auto valid = qualifyGraphImport(changes.projections, link.projection, *backing, user); !valid) return valid;
        if (user.slot->output_range) stageProducts(changes, link.projection, *user.slot, *backing);
        auto& projection = changes.projections.try_emplace(link.projection, ProjectionBindings{*backing}).first->second;
        link.subscription = projection.users.emplace(user);
        return {};
    }

    void ResourceRegistry::stageProducts(
        BindingChanges::Resource& changes, std::uint32_t projection, const detail::ImportedResourceSlot& operand,
        const ProjectionFamily& candidate
    ) noexcept
    {
        const auto& entry = *entries_.find(changes.storage);
        for (const auto& slice : candidate.buffers)
        {
            if (!slice.buffer) continue;
            if (entry.product_buffers.contains(slice.buffer) || changes.product_buffers.contains(slice.buffer)) continue;
            const auto* device = deviceOf(changes.storage);
            changes.product_buffers.emplace(
                slice.buffer, ProductBufferUse{{device->makeFrontiers()}, {device->makeFrontiers()}}
            );
        }
        for (const auto& exported : operand.exports)
        {
            const auto matches = [&](const BufferProduct& product)
            {
                return product.projection == projection && product.range == exported.bytes;
            };
            const bool known = std::ranges::any_of(entry.products, matches) ||
                std::ranges::any_of(changes.products, matches);
            if (known) continue;
            auto& product = changes.products.emplace_back(BufferProduct{projection, exported.bytes, {}});
            for (std::size_t i = 0; i < product.samples.size(); ++i)
                product.samples[i].backing = candidate.buffers[i].part(product.range);
        }
    }

    RenderResult<void> ResourceRegistry::reserveBindings(const BindingChanges& pending) noexcept
    {
        const auto reserve = [](auto& rows, std::size_t needed, std::size_t limit) noexcept
        {
            const auto current = rows.capacity();
            if (current >= needed)
            {
                rows.reserve(needed); // A SlotMap also reserves its dense values and reverse indices.
                return;
            }
            const auto growth = std::min(current / 2, limit - current);
            rows.reserve(std::max(needed, current + growth));
        };
        for (const auto& changes : pending.rows)
        {
            auto& entry = *entries_.find(changes.storage);
            const bool too_many_products = changes.products.size() > entry.products.max_size() - entry.products.size();
            const bool too_many_buffers =
                changes.product_buffers.size() > entry.product_buffers.max_size() - entry.product_buffers.size();
            if (too_many_products || too_many_buffers) return cxx::unexpected(RenderError{kStorageCapacity});
            reserve(entry.products, entry.products.size() + changes.products.size(), entry.products.max_size());
            for (const auto& [projection, row] : changes.projections)
            {
                const auto installed = entry.graph_bindings.find(projection);
                if (installed == entry.graph_bindings.end()) continue;
                auto& family = installed->second;
                constexpr std::size_t limit = decltype(family.users)::INVALID_INDEX;
                const auto occupied = family.users.size() + family.retired_slots;
                if (row.users.size() > limit - occupied) return cxx::unexpected(RenderError{kStorageCapacity});
                reserve(family.users, occupied + row.users.size(), limit);
            }
        }
        return {};
    }

    void ResourceRegistry::publishBindings(
        BindingChanges& pending,
        std::span<runtime_detail::ProjectionSubscription> links, std::vector<ResourceInstanceId>& installed
    ) noexcept
    {
        for (auto& changes : pending.rows)
        {
            auto& entry = *entries_.find(changes.storage);
            entry.graph_bindings.merge(changes.projections);
            std::ranges::move(changes.products, std::back_inserter(entry.products));
            entry.product_buffers.merge(changes.product_buffers);
        }
        // A transferred projection keeps its provisional keys. Existing projections issue their own keys.
        for (auto& link : links)
        {
            const auto owner = std::ranges::find(pending.rows, link.storage, &BindingChanges::Resource::storage);
            if (owner == pending.rows.end()) std::terminate();
            const auto& remaining = owner->projections;
            const auto staged = remaining.find(link.projection);
            if (staged == remaining.end()) continue;
            const auto* reader = staged->second.users.find(link.subscription);
            if (!reader) std::terminate();
            auto& installed = entries_.find(link.storage)->graph_bindings.find(link.projection)->second;
            link.subscription = installed.users.emplace(*reader);
        }
        installed.swap(pending.resources);
        pending.rows.clear();
        pending.created.clear();
    }
}
