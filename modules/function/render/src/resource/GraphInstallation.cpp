#include <lux/engine/render/resource/GraphInstallation.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <algorithm>
#include <iterator>

namespace lux::render
{
    ResourceRegistry::GraphInstallation::GraphInstallation(
        ResourceRegistry& registry, runtime_detail::JobExecution& execution
    ) noexcept : registry_(registry), execution_(execution)
    {
        if (registry_.installation_) std::terminate();
        registry_.installation_ = this;
    }

    ResourceRegistry::GraphInstallation::~GraphInstallation() noexcept
    {
        if (registry_.installation_ == this) registry_.installation_ = nullptr;
    }

    ResourceRegistry::GraphInstallation::StorageChanges&
    ResourceRegistry::GraphInstallation::changesFor(ResourceInstanceId storage) noexcept
    {
        const auto row = std::ranges::find(changes_, storage, &StorageChanges::storage);
        return row == changes_.end() ? changes_.emplace_back(StorageChanges{storage}) : *row;
    }

    bool ResourceRegistry::GraphInstallation::holds(ResourceInstanceId storage) const noexcept
    {
        return std::ranges::find(changes_, storage, &StorageChanges::storage) != changes_.end();
    }

    RenderResult<void>
    ResourceRegistry::GraphInstallation::resolve(runtime_detail::BoundStorageProjection& link, bool may_access) noexcept
    {
        if (link.publication)
        {
            auto value = registry_.selectProduct(link.storage, link.projection, *link.publication);
            if (!value) return cxx::unexpected(value.error());
            changesFor(link.storage);
            execution_.bindResource(link.resource, UINT32_MAX, *value);
            return {};
        }
        const GraphBindingUse user{
            execution_.binding_identity, link.resource, link.output_range, may_access, link.canonical_queue
        };
        auto backing = resolveBacking(link, user);
        if (!backing) return cxx::unexpected(backing.error());
        auto& changes = changesFor(link.storage);
        if (auto valid = qualifyGraphImport(changes.projections, link.projection, *backing, user); !valid) return valid;
        prepareProducts(changes, link, *backing);
        auto& row = changes.projections.try_emplace(link.projection, ProjectionBindings{*backing}).first->second;
        link.subscription = row.users.emplace(user);
        return {};
    }

    void ResourceRegistry::GraphInstallation::prepareProducts(
        StorageChanges& changes, const runtime_detail::BoundStorageProjection& link, const ProjectionFamily& candidate
    ) noexcept
    {
        if (!link.output_range) return;
        const auto& entry = *registry_.entries_.find(link.storage);
        for (const auto& slice : candidate.buffers)
        {
            if (!slice.buffer) continue;
            const auto contains_buffer = [&](const auto& buffers)
            {
                return std::ranges::find(buffers, slice.buffer, &ProductBufferUse::buffer) != buffers.end();
            };
            if (contains_buffer(entry.product_buffers) || contains_buffer(changes.product_buffers)) continue;
            const auto* device = registry_.deviceOf(link.storage);
            changes.product_buffers.push_back({slice.buffer, device->makeFrontiers(), device->makeFrontiers()});
        }
        for (const auto& exported : link.exports)
        {
            const auto matches = [&](const BufferProduct& product)
            {
                return product.projection == link.projection && product.range == exported.bytes;
            };
            const bool known = std::ranges::any_of(entry.products, matches) ||
                std::ranges::any_of(changes.products, matches);
            if (known) continue;
            auto& product = changes.products.emplace_back(BufferProduct{link.projection, exported.bytes, {}});
            for (std::size_t i = 0; i < product.samples.size(); ++i)
                product.samples[i].backing = candidate.buffers[i].part(product.range);
        }
    }

    RenderResult<void> ResourceRegistry::GraphInstallation::reservePublication() noexcept
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
        for (const auto& changes : changes_)
        {
            auto& entry = *registry_.entries_.find(changes.storage);
            const bool too_many_products = changes.products.size() > entry.products.max_size() - entry.products.size();
            const bool too_many_buffers =
                changes.product_buffers.size() > entry.product_buffers.max_size() - entry.product_buffers.size();
            if (too_many_products || too_many_buffers) return cxx::unexpected(RenderError{kStorageCapacity});
            reserve(entry.products, entry.products.size() + changes.products.size(), entry.products.max_size());
            reserve(entry.product_buffers, entry.product_buffers.size() + changes.product_buffers.size(),
                entry.product_buffers.max_size());
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

    void ResourceRegistry::GraphInstallation::publish() noexcept
    {
        for (auto& changes : changes_)
        {
            auto& entry = *registry_.entries_.find(changes.storage);
            entry.graph_bindings.merge(changes.projections);
            std::ranges::move(changes.products, std::back_inserter(entry.products));
            std::ranges::move(changes.product_buffers, std::back_inserter(entry.product_buffers));
        }
        // A transferred projection keeps its provisional keys. Existing projections issue their own keys.
        for (auto& link : execution_.storage_bindings)
        {
            if (link.publication) continue;
            const auto owner = std::ranges::find(changes_, link.storage, &StorageChanges::storage);
            if (owner == changes_.end()) std::terminate();
            const auto& remaining = owner->projections;
            const auto staged = remaining.find(link.projection);
            if (staged == remaining.end()) continue;
            const auto* reader = staged->second.users.find(link.subscription);
            if (!reader) std::terminate();
            auto& installed = registry_.entries_.find(link.storage)->graph_bindings.find(link.projection)->second;
            link.subscription = installed.users.emplace(*reader);
        }
        registry_.installation_ = nullptr;
    }
}
