#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/resource/FrameDataCall.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/InstalledFrameResource.hpp>
#include <algorithm>
#include <lux/engine/render/resource/InstalledProjection.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/runtime/commands/detail/CommandDispatchContext.hpp>
#include <exception>
#include <limits>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>

namespace lux::render
{
    namespace
    {
        using ImageFamily = std::array<nri::Texture*, 3>;

        bool sharesImage(const ImageFamily& a, const ImageFamily& b) noexcept
        {
            for (auto* image : a)
                if (image && std::ranges::find(b, image) != b.end()) return true;
            return false;
        }

        bool includesFrame(const GraphImagePublication& update, std::size_t slot) noexcept
        {
            return update.frame == UINT32_MAX || update.frame == slot;
        }

        ImageFamily selectedImages(
            ImageFamily current, std::uint32_t projection, std::span<const GraphImagePublication> updates,
            std::uint32_t count
        ) noexcept
        {
            for (const auto& update : updates)
                if (update.projection == projection)
                    for (std::uint32_t i = 0; i < count; ++i)
                        if (includesFrame(update, i)) current[i] = update.image;
            return current;
        }

        bool hasBufferFields(const detail::ResourceInput& value) noexcept
        {
            return value.buffer || value.offset || value.size || !value.addressed_buffers.empty() ||
                value.addressed_revision;
        }

        bool validBufferSlice(const detail::ResourceInput& value, const detail::DeviceSession& device) noexcept
        {
            const auto capacity = value.buffer ? device.nriCore().GetBufferDesc(*value.buffer).size : UINT64_MAX;
            return value.offset <= capacity && value.size <= capacity - value.offset;
        }

        bool validQueueUses(
            bool has_backing, detail::ResourceQueueUses uses, const detail::DeviceSession& device
        ) noexcept
        {
            const detail::SubmissionFrontiers* frontiers[]{uses.preparation, uses.accepted};
            for (const auto* frontier : frontiers)
            {
                if (!frontier) continue;
                if (!has_backing || !frontier->belongsTo(&device, device.queueCount())) return false;
                for (std::size_t q = 0; q < frontier->size(); ++q)
                    if (frontier->serials()[q] > device.physicalQueue(q).submitted()) return false;
            }
            return true;
        }
    }

    std::array<ResourceRegistry::ProjectionBuffer, 3> ResourceRegistry::proposedBuffers(
        std::uint32_t projection, const ProjectionFamily& family, std::span<const GraphBufferPublication> updates
    ) noexcept
    {
        auto slices = family.buffers;
        for (const auto& update : updates)
        {
            if (update.projection != projection) continue;
            for (std::uint32_t i = 0; i < family.frame_count; ++i)
                if (update.frame == UINT32_MAX || update.frame == i)
                    slices[i] = {update.value.buffer, {update.value.offset, update.value.size}};
        }
        return slices;
    }

    RenderResult<void> ResourceRegistry::qualifyBufferUses(
        const ProjectionMap& projections, const BufferClaim& claim, std::span<const GraphBufferPublication> updates
    ) noexcept
    {
        const auto& [projection, buffers, user] = claim;
        const auto overlaps = [](GraphBufferRange a, GraphBufferRange b) noexcept
        {
            return a.offset <= b.offset ? b.offset - a.offset < a.size : a.offset - b.offset < b.size;
        };
        const auto physical = [](ProjectionBuffer slice, std::optional<GraphBufferRange> writer) noexcept
        {
            if (writer) return GraphBufferRange{slice.bytes.offset + writer->offset, writer->size};
            return slice.bytes;
        };
        const auto& requested_write = user.slot->output_range;
        for (const auto& slice : buffers)
        {
            if (slice.bytes.size > UINT64_MAX - slice.bytes.offset)
                return cxx::unexpected(RenderError{kGraphInvalidRange});
            if (!requested_write) continue;
            const auto range = *requested_write;
            if (!slice.buffer || range.offset > slice.bytes.size || range.size > slice.bytes.size - range.offset)
                return cxx::unexpected(RenderError{kGraphInvalidRange});
        }
        for (const auto& [other_projection, binding] : projections)
        {
            const bool same_projection = projection == other_projection;
            const auto other_slices = proposedBuffers(other_projection, binding.backing, updates);
            const auto previous = same_projection ? buffers : std::span(other_slices).first(binding.backing.frame_count);
            for (const auto& bound : binding.users)
            {
                const auto& bound_write = bound.slot->output_range;
                if (!bound_write && !requested_write) continue;
                const bool same_job = bound.binding.job == user.binding.job;
                const bool replacing = same_job && bound.binding.generation != user.binding.generation;
                const bool same_use = same_job && bound.slot->resource == user.slot->resource;
                // Disabled command bodies still record their planned barriers.
                const bool compatible_barriers = bound.slot->boundary.canShare(user.slot->boundary);
                const bool exclusive_paths = same_projection && compatible_barriers && same_job && bound_write &&
                    requested_write && (!bound.may_access || !user.may_access);
                if (replacing || same_use || exclusive_paths) continue;
                const auto conflict = same_job ? kGraphConflictingOutput : kJobBusy;
                const bool conflicts = same_projection &&
                    (!bound_write || !requested_write || overlaps(*bound_write, *requested_write));
                if (conflicts)
                    return cxx::unexpected(RenderError{conflict});
                for (const auto& a : buffers)
                {
                    if (!a.buffer) continue;
                    for (const auto& b : previous)
                    {
                        if (a.buffer != b.buffer) continue;
                        if (overlaps(physical(a, requested_write), physical(b, bound_write)))
                            return cxx::unexpected(RenderError{conflict});
                        // Separate Job writers can take turns at the same native boundary under their product pins.
                        const bool can_take_turns = !same_job && bound_write && requested_write &&
                            bound.slot->boundary.use == user.slot->boundary.use;
                        if (!compatible_barriers && !can_take_turns) return cxx::unexpected(RenderError{conflict});
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> ResourceRegistry::qualifyGraphImport(
        const ProjectionMap& projections,
        std::uint32_t projection,
        const ProjectionFamily& backing,
        const GraphBindingUse& user
    ) noexcept
    {
        if (std::ranges::any_of(backing.images, [](const auto* image) { return image != nullptr; }))
        {
            for (const auto& [other, row] : projections)
                if (other != projection && sharesImage(backing.images, row.backing.images))
                    return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto buffers = std::span(backing.buffers).first(backing.frame_count);
        if (auto valid = qualifyBufferUses(projections, {projection, buffers, user}); !valid) return valid;
        const auto existing = projections.find(projection);
        if (existing != projections.end() && existing->second.backing != backing)
            return cxx::unexpected(RenderError{kStorageMismatch});
        return {};
    }

    RenderResult<ResourceRegistry::ProjectionFamily>
    ResourceRegistry::resolveBacking(
        const runtime_detail::ProjectionSubscription& link, const GraphBindingUse& user
    ) noexcept
    {
        auto* entry = entries_.find(link.storage);
        if (!entry || !entry->instance) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto frame_count = static_cast<std::uint32_t>(user.target->frames.size());
        const auto installed = entry->graph_bindings.find(link.projection);
        if (installed != entry->graph_bindings.end())
        {
            const auto buffers = std::span(installed->second.backing.buffers).first(frame_count);
            if (auto valid = qualifyBufferUses(entry->graph_bindings, {link.projection, buffers, user}); !valid)
                return cxx::unexpected(valid.error());
        }
        const auto& ops = *entry->type->factory.storage().bindings();
        std::array<detail::ResourceInput, 3> slices{};
        auto output = std::span(slices).first(frame_count);
        const auto& description = user.target->graph.resources()[user.slot->resource.index].description;
        if (auto resolved = ops.resolveGraphImport(entry->instance, link.projection, description, output); !resolved)
            return cxx::unexpected(resolved.error());

        // An extension may have created other resources during resolution. Reacquire the row by identity.
        entry = entries_.find(link.storage);
        ProjectionFamily candidate{frame_count, std::holds_alternative<GraphImageDesc>(description)};
        auto& buffers = candidate.buffers;
        auto& images = candidate.images;
        auto& queue_uses = candidate.queue_uses;
        const bool is_image = candidate.image_projection;
        for (std::size_t i = 0; i < output.size(); ++i)
        {
            if ((is_image && hasBufferFields(output[i])) || (!is_image && output[i].image))
                return cxx::unexpected(RenderError{kStorageMismatch});
            images[i] = output[i].image;
            queue_uses[i] = output[i].queue_uses;
            if (!validQueueUses(images[i] || output[i].buffer, queue_uses[i], *deviceOf(link.storage)))
                return cxx::unexpected(RenderError{kStorageMismatch});
            if (images[i])
            {
                const auto& compiled = user.target->graph.resources()[user.slot->resource.index];
                if (compiled.concurrent_image_read && (!queue_uses[i].preparation || !queue_uses[i].accepted))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                if (auto valid = user.target->qualifyImage(user.slot->resource, *images[i]); !valid)
                    return cxx::unexpected(valid.error());
            }
            if (!is_image && !validBufferSlice(output[i], user.target->device))
                return cxx::unexpected(RenderError{kGraphInvalidRange});
            buffers[i] = {output[i].buffer, {output[i].offset, output[i].size}};
        }
        if (auto valid = qualifyGraphImport(entry->graph_bindings, link.projection, candidate, user); !valid)
            return cxx::unexpected(valid.error());
        // Only an owner publication may replace a backing that is still visible through an issued receipt.
        for (const auto& product : entry->products)
        {
            if (product.projection != link.projection) continue;
            for (std::size_t i = 0; i < output.size(); ++i)
                if (buffers[i].part(product.range) != product.samples[i].backing)
                    return cxx::unexpected(RenderError{kStorageMismatch});
        }
        for (std::uint32_t slot = 0; slot < output.size(); ++slot)
            user.target->bind(user.slot->resource, slot, output[slot]);
        return candidate;
    }

    void ResourceRegistry::unbindGraphProjection(
        const runtime_detail::ProjectionSubscription& link, const detail::ExecutionBindingId& identity
    ) noexcept
    {
        auto& projections = entries_.find(link.storage)->graph_bindings;
        const auto found = projections.find(link.projection);
        if (found == projections.end()) std::terminate();
        auto& users = found->second.users;
        const auto* reader = users.find(link.subscription);
        const bool current = reader && reader->binding == identity;
        if (!current) std::terminate();
        const bool was_writer = reader->slot->output_range.has_value();
        if (link.subscription.gen == std::numeric_limits<decltype(link.subscription.gen)>::max() - 1)
            ++found->second.retired_slots;
        users.erase(link.subscription);
        if (users.empty()) projections.erase(found);
        if (was_writer) pruneBufferProducts(link.storage);
    }

    bool ResourceRegistry::hasGraphInputUsers(ResourceInstanceId id, std::uint32_t projection) const noexcept
    {
        const auto* entry = entries_.find(id);
        if (!entry) return false;
        const auto binding = entry->graph_bindings.find(projection);
        return binding != entry->graph_bindings.end() && !binding->second.users.empty();
    }

    RenderResult<std::size_t> ResourceRegistry::publishGraphInput(
        ResourceInstanceId storage, std::uint32_t projection, std::uint32_t frame, const detail::ResourceInput& value
    ) noexcept
    {
        auto* entry = entries_.find(storage);
        if (!entry || !entry->instance) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto found = entry->graph_bindings.find(projection);
        if (value.image || (found != entry->graph_bindings.end() && found->second.backing.image_projection))
        {
            if (hasBufferFields(value)) return cxx::unexpected(RenderError{kStorageMismatch});
            const GraphImagePublication update{projection, frame, value.image, value.queue_uses};
            return publishGraphImages(storage, {&update, 1});
        }
        const GraphBufferPublication update{projection, frame, value};
        return publishGraphBuffers(storage, {&update, 1});
    }

    RenderResult<std::size_t>
    ResourceRegistry::publishGraphBuffers(ResourceInstanceId storage, std::span<const GraphBufferPublication> updates) noexcept
    {
        auto* entry = entries_.find(storage);
        if (!entry || !entry->instance) return cxx::unexpected(RenderError{kStorageIdentity});
        if (updates.empty()) return 0;
        if (pending_bindings_ && pending_bindings_->holds(storage)) return cxx::unexpected(RenderError{kDeviceBusy});
        auto& device = entry->storage_set->device_;
        const auto count = device.frameProgress().frameCount();
        for (std::size_t i = 0; i < updates.size(); ++i)
        {
            const auto& update = updates[i];
            const auto& value = update.value;
            if (update.frame != UINT32_MAX && update.frame >= count) return cxx::unexpected(RenderError{kFrameContract});
            if (value.image || !validQueueUses(value.buffer != nullptr, value.queue_uses, device.session()))
                return cxx::unexpected(RenderError{kStorageMismatch});
            if (!validBufferSlice(value, device.session())) return cxx::unexpected(RenderError{kGraphInvalidRange});
            for (const auto& prior : updates.first(i))
            {
                const bool overlaps = prior.frame == UINT32_MAX || update.frame == UINT32_MAX || prior.frame == update.frame;
                if (prior.projection == update.projection && overlaps) return cxx::unexpected(RenderError{kStorageContract});
            }
            if (auto valid = qualifyProductReplacement(storage, update, count); !valid)
                return cxx::unexpected(valid.error());
        }

        for (const auto& update : updates)
        {
            const auto found = entry->graph_bindings.find(update.projection);
            if (found == entry->graph_bindings.end()) continue;
            const auto& binding = found->second;
            if (binding.backing.image_projection) return cxx::unexpected(RenderError{kStorageMismatch});
            const auto candidate = proposedBuffers(update.projection, binding.backing, updates);
            for (const auto& user : binding.users)
            {
                if (!entry->product_buffers.empty() && candidate != binding.backing.buffers)
                {
                    const BufferClaim claim{update.projection, std::span(candidate).first(count), user};
                    if (auto valid = qualifyBufferUses(entry->graph_bindings, claim, updates); !valid)
                        return cxx::unexpected(valid.error());
                }
                for (std::uint32_t f = 0; f < count; ++f)
                {
                    if (update.frame != UINT32_MAX && update.frame != f) continue;
                    const auto& reader = user.target->frames[f];
                    if (!reader.frozen_for_submission) continue;
                    const auto& selected = reader.bindings[user.slot->resource.index];
                    if (!selected.sameBacking(update.value) || !selected.sameAddresses(update.value))
                        return cxx::unexpected(RenderError{kDeviceBusy});
                }
            }
        }

        // Allocate tracking for new writer backings after qualification and before changing any issued version.
        for (const auto& update : updates)
        {
            const auto found = entry->graph_bindings.find(update.projection);
            if (found != entry->graph_bindings.end() && found->second.hasWriter())
                reserveProductBuffer(storage, update.value.buffer);
        }
        std::size_t updated = 0;
        for (const auto& update : updates)
        {
            adoptProductReplacement(storage, update, count);
            const auto found = entry->graph_bindings.find(update.projection);
            if (found == entry->graph_bindings.end()) continue;
            auto& binding = found->second;
            const auto& value = update.value;
            for (std::uint32_t f = 0; f < count; ++f)
            {
                if (update.frame != UINT32_MAX && update.frame != f) continue;
                binding.backing.buffers[f] = {value.buffer, {value.offset, value.size}};
                binding.backing.queue_uses[f] = value.queue_uses;
            }
            for (const auto& user : binding.users) user.target->bind(user.slot->resource, update.frame, value);
            updated += binding.users.size();
        }
        if (!entry->product_buffers.empty()) pruneBufferProducts(storage);
        return updated;
    }

    RenderResult<std::size_t>
    ResourceRegistry::publishGraphImages(
        ResourceInstanceId storage, std::span<const GraphImagePublication> updates
    ) noexcept
    {
        auto* entry = entries_.find(storage);
        if (!entry || !entry->instance) return cxx::unexpected(RenderError{kStorageIdentity});
        if (!updates.empty() && pending_bindings_ && pending_bindings_->holds(storage))
            return cxx::unexpected(RenderError{kDeviceBusy});
        auto& bindings = entry->graph_bindings;
        for (std::size_t i = 0; i < updates.size(); ++i)
        {
            const auto& update = updates[i];
            if (!validQueueUses(update.image, update.uses, *deviceOf(storage)))
                return cxx::unexpected(RenderError{kStorageMismatch});
            for (const auto& prior : updates.first(i))
            {
                if (prior.projection != update.projection && update.image && prior.image == update.image)
                    return cxx::unexpected(RenderError{kStorageMismatch});
                const bool overlaps = update.frame == UINT32_MAX || includesFrame(prior, update.frame);
                if (prior.projection == update.projection && overlaps)
                    return cxx::unexpected(RenderError{kStorageContract});
            }
            const auto current = bindings.find(update.projection);
            if (current == bindings.end())
            {
                const auto product = std::ranges::find(entry->products, update.projection, &BufferProduct::projection);
                if (product != entry->products.end()) return cxx::unexpected(RenderError{kStorageMismatch});
                continue;
            }
            const auto& family = current->second;
            const auto count = family.backing.frame_count;
            if (update.frame != UINT32_MAX && update.frame >= count)
                return cxx::unexpected(RenderError{kFrameContract});
            for (const auto& user : family.users)
            {
                const auto& reader = *user.target;
                const auto& graph_resource = reader.graph.resources()[user.slot->resource.index];
                if (!std::holds_alternative<GraphImageDesc>(graph_resource.description))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                for (std::uint32_t f = 0; f < count; ++f)
                {
                    if (!includesFrame(update, f)) continue;
                    if (family.backing.images[f] == update.image && family.backing.queue_uses[f] == update.uses) continue;
                    if (reader.frames[f].frozen_for_submission) return cxx::unexpected(RenderError{kDeviceBusy});
                }
                if (update.image)
                {
                    if (graph_resource.concurrent_image_read && (!update.uses.preparation || !update.uses.accepted))
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    auto valid = reader.qualifyImage(user.slot->resource, *update.image);
                    if (!valid) return cxx::unexpected(valid.error());
                }
            }
            if (!update.image) continue;
            for (const auto& [projection, other] : bindings)
            {
                if (projection == update.projection) continue;
                const auto final = selectedImages(other.backing.images, projection, updates, count);
                if (std::ranges::find(final, update.image) != final.end())
                    return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        std::size_t updated = 0;
        for (const auto& update : updates)
        {
            const auto current = bindings.find(update.projection);
            if (current == bindings.end()) continue;
            auto& family = current->second;
            const auto count = family.backing.frame_count;
            for (std::uint32_t f = 0; f < count; ++f)
            {
                if (!includesFrame(update, f)) continue;
                if (family.backing.images[f] == update.image && family.backing.queue_uses[f] == update.uses) continue;
                family.backing.images[f] = update.image;
                family.backing.queue_uses[f] = update.uses;
                const detail::ResourceInput input{.image = update.image, .queue_uses = update.uses};
                for (const auto& user : family.users)
                    user.target->bind(user.slot->resource, f, input);
            }
            updated += family.users.size();
        }
        return updated;
    }

    void runtime_detail::finishResourceCommand(ResourceRegistry& registry, ResourceInstanceId id) noexcept
    {
        if (!id.isNull()) registry.finishCommand(id);
    }

    RenderResult<void*>
    ResourceRegistry::beginCommand(ResourceInstanceId id, bool exclusive) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance || entry->storage_set->closed_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return beginCommand(*entry, exclusive);
    }

    RenderResult<void*> ResourceRegistry::beginCommand(Entry& entry, bool exclusive) noexcept
    {
        const bool conflicting_command = entry.exclusive_command || (exclusive && entry.command_uses != 0);
        if (conflicting_command)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        if (entry.command_uses == UINT32_MAX) std::terminate();
        ++entry.command_uses;
        entry.exclusive_command = exclusive;
        return entry.instance;
    }

    RenderResult<bool> ResourceRegistry::selectFrameUpdate(
        const FrameData::Batch& batch, const FrameData& packet, detail::FrameDataCall& selected
    ) noexcept
    {
        const auto id = batch.target.instance;
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance || entry->storage_set->closed_) return cxx::unexpected(RenderError{kStorageIdentity});
        if (auto error = entry->storage_set->device_.admissionError(); error.type) return cxx::unexpected(error);
        const auto& registration = entry->type->factory;
        if (!registration.applyFrame() || !std::ranges::equal(registration.channels(), batch.channels))
            return cxx::unexpected(RenderError{kFrameContract});
        const bool per_run = registration.descriptor->lifetime == EResourceContentLifetime::PER_RUN;
        bool supplied = per_run;
        for (std::size_t i = 0; !supplied && i < batch.channels.size(); ++i)
            supplied = packet.column(batch.first_column + i).count != 0;
        if (!supplied) return false;
        auto owner = beginCommand(*entry, false);
        if (!owner) return cxx::unexpected(owner.error());
        selected = {id, *owner, registration.applyFrame(), batch.first_column, per_run};
        return true;
    }

    bool ResourceRegistry::hasBorrows(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && (entry->command_uses || (pending_bindings_ && pending_bindings_->protects(id)) ||
            entry->storage_set->device_.jobUsesResource(id));
    }

    void ResourceRegistry::collectFrameDependencies(
        std::span<const ResourceInstanceId> roots, std::vector<ResourceInstanceId>& result
    ) const noexcept
    {
        // The candidate already owns the sorted roots. Dependencies become immutable on publication.
        result.clear();
        const auto visit = [&](ResourceInstanceId id) noexcept
        {
            const auto* entry = entries_.find(id);
            if (!entry) std::terminate();
            for (auto dependency : entry->dependencies)
            {
                if (std::ranges::binary_search(roots, detail::resourceOrder(dependency), {}, detail::resourceOrder))
                    continue;
                if (std::ranges::find(result, dependency) == result.end()) result.push_back(dependency);
            }
        };
        for (auto root : roots) visit(root);
        for (std::size_t i = 0; i < result.size(); ++i) visit(result[i]);
        std::ranges::sort(result, {}, detail::resourceOrder);
    }

    bool ResourceRegistry::hasDependents(ResourceInstanceId id) const noexcept
    {
        return std::ranges::any_of(entries_, [id](const Entry& owner) noexcept
        {
            return std::ranges::find(owner.dependencies, id) != owner.dependencies.end();
        });
    }

    bool ResourceRegistry::hasSurvivingDependencyOn(const ResourceSet& set) const noexcept
    {
        for (const auto& owner : entries_)
        {
            if (owner.storage_set == &set && owner.instance) continue;
            for (auto id : owner.dependencies)
            {
                const auto* target = entries_.find(id);
                if (target && target->storage_set == &set && target->instance) return true;
            }
        }
        return false;
    }

    RenderResult<ResourceInstanceId> ResourceCreateContext::require(ResourceNameView dependency) noexcept
    {
        return resources.requireDuringConstruction(*this, constructing_, resources.typeId(dependency));
    }

    RenderResult<ResourceInstanceId> ResourceRegistry::requireDuringConstruction(
        ResourceCreateContext& context, ResourceInstanceId consumer, ResourceTypeId type
    ) noexcept
    {
        auto* entry = entries_.find(consumer);
        const auto* definition = factory(type);
        const bool constructing = entry && !entry->instance && entry->position == SIZE_MAX;
        if (!constructing || !definition) return cxx::unexpected(RenderError{kStorageContract});
        auto* scope = entry->storage_set->ownerFor(definition->descriptor->domain);
        if (!scope) return cxx::unexpected(RenderError{kStorageMismatch});
        auto dependency = scope->getOrCreate(context, type);
        if (!dependency) return cxx::unexpected(dependency.error());

        auto& dependencies = entries_.find(consumer)->dependencies;
        if (std::ranges::find(dependencies, *dependency) == dependencies.end())
            dependencies.push_back(*dependency);
        return *dependency;
    }

    bool ResourceRegistry::hasExclusiveCommand(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->exclusive_command;
    }

    detail::DeviceEntry* ResourceRegistry::deviceOwner(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? &entry->storage_set->device_ : nullptr;
    }

    const detail::DeviceSession* ResourceRegistry::deviceOf(ResourceInstanceId id) const noexcept
    {
        const auto* owner = deviceOwner(id);
        return owner ? &owner->session() : nullptr;
    }

    bool ResourceRegistry::canReference(ResourceInstanceId owner, ResourceInstanceId dependency) const noexcept
    {
        const auto* target = entries_.find(owner);
        const auto* source = entries_.find(dependency);
        const bool has_instances = target && target->instance && source && source->instance;
        if (!has_instances)
        {
            return false;
        }
        if (&target->storage_set->device_ != &source->storage_set->device_)
        {
            return false;
        }
        for (const auto* domain = target->storage_set; domain; domain = domain->parent_)
        {
            if (domain == source->storage_set)
            {
                return true;
            }
        }
        return false;
    }

    void ResourceRegistry::finishCommand(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->command_uses)
        {
            std::terminate();
        }
        if (--entry->command_uses == 0)
        {
            entry->exclusive_command = false;
        }
        auto* scope = entry->storage_set;
        if (scope->closed_ && !scope->hasBorrows())
        {
            auto& retired = scope->parent_->retiring_jobs_;
            const auto found = std::ranges::find(retired, scope, &std::unique_ptr<ResourceSet>::get);
            if (found != retired.end())
            {
                auto released = std::move(*found);
                retired.erase(found);
            }
        }
    }

    ResourceRegistry::~ResourceRegistry() noexcept
    {
        if (!entries_.empty())
        {
            std::terminate(); // actual Sets must close before their borrowed index
        }
    }

    ResourceTypeId ResourceRegistry::declarationOf(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? entry->type->id : ResourceTypeId{};
    }

    ResourceTypeId ResourceRegistry::typeId(ResourceNameView type) const noexcept
    {
        const auto found = types_by_name_.find(type.name());
        return found == types_by_name_.end() ? ResourceTypeId{} : found->second;
    }

    const ResourceRegistration* ResourceRegistry::factory(ResourceTypeId type) const noexcept
    {
        const auto* entry = types_.find(type);
        return entry ? &entry->factory : nullptr;
    }

    const ResourceRegistration* ResourceRegistry::factory(ResourceNameView type) const noexcept
    {
        const auto index = typeId(type);
        return index.isNull() ? nullptr : &types_.find(index)->factory;
    }

    const ResourceRegistration* ResourceRegistry::factoryOf(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? &entry->type->factory : nullptr;
    }

    RenderResult<const ResourceRegistration*>
    ResourceRegistry::qualify(ResourceTypeId type, EResourceDomain domain, GpuResourceFeatures available) const noexcept
    {
        const auto result = qualifyType(type, domain, available);
        if (!result) return cxx::unexpected(result.error());
        return &(*result)->factory;
    }

    RenderResult<const ResourceRegistry::Type*>
    ResourceRegistry::qualifyType(ResourceTypeId type, EResourceDomain domain, GpuResourceFeatures available) const noexcept
    {
        const auto* definition = types_.find(type);
        if (!definition)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        const bool is_wrong_domain = definition->factory.descriptor->domain != domain;
        if (is_wrong_domain)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (!definition->factory.descriptor->supports(available))
        {
            return cxx::unexpected(RenderError{kStorageCapability});
        }
        if (definition->factory.storage().config_type.isValid() && !definition->configuration)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        return definition;
    }

    RenderResult<void> ResourceRegistry::validateFactory(const ResourceRegistration& factory) const noexcept
    {
        if (auto valid = validateResourceRegistration(factory); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (!typeId(factory.descriptor->type).isNull())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    RenderResult<void>
    ResourceRegistry::validateFactory(const ResourceRegistration& factory, const ConfigValue& configuration)
        const noexcept
    {
        if (auto valid = validateFactory(factory); !valid)
        {
            return valid;
        }
        const auto expected = factory.storage().config_type;
        const bool is_missing_config = expected.isValid() && !configuration;
        if (is_missing_config) return cxx::unexpected(RenderError{kStorageContract});
        const auto actual = configuration.type();
        const bool is_wrong_config_type = configuration && (expected != actual || expected.name() != actual.name());
        if (is_wrong_config_type)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    ResourceRegistry::Type::Type(ResourceRegistration source, ConfigValue config) noexcept
        : descriptor(*source.descriptor), frame_inputs(descriptor.frame_inputs.begin(), descriptor.frame_inputs.end()),
          frame_channels(source.channels().begin(), source.channels().end()),
          factory(std::move(source)), configuration(std::move(config))
    {
        const auto own_name = [this](std::string_view text) noexcept -> std::string_view
        {
            return text.empty() ? std::string_view{} : std::string_view{names.emplace_back(text)};
        };
        const auto own_type = [&](cxx::basic_type_info& type) noexcept
        {
            type = {type.hash(), own_name(type.name())};
        };

        descriptor.type = ResourceNameView{own_name(descriptor.type.name())};
        descriptor.description = own_name(descriptor.description);
        own_type(descriptor.binding_type);
        for (auto& type : frame_inputs) own_type(type);
        descriptor.frame_inputs = frame_inputs;
        factory.descriptor = &descriptor;

        if (!frame_channels.empty())
        {
            // Registration qualified one declaration and the exact element identity of every column.
            auto declaration = frame_channels.front().declaration;
            own_type(declaration);
            for (std::size_t i = 0; i < frame_channels.size(); ++i)
            {
                frame_channels[i].declaration = declaration;
                frame_channels[i].element = frame_inputs[i];
            }
        }
        factory.frame_channels_ = frame_channels;

        auto& storage = factory.storage_;
        own_type(storage.config_type);
        own_type(storage.implementation_type);
        if (storage.binding_ops && storage.binding_ops->cpu_view) own_type(storage.binding_ops->cpu_view->type);
    }

    ResourceTypeId ResourceRegistry::publishFactory(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        const auto id = types_.emplace(std::move(factory), std::move(configuration));
        auto& row = *types_.find(id);
        row.id = id;
        types_by_name_.emplace(std::string(row.descriptor.type.name()), id);
        return id;
    }

    RenderResult<ResourceTypeId>
    ResourceRegistry::installFactory(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        if (auto valid = validateFactory(factory, configuration); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return publishFactory(std::move(factory), std::move(configuration));
    }

    ResourceInstanceId ResourceRegistry::reserve(ResourceSet& set, const Type& type) noexcept
    {
        return entries_.emplace(Entry{&set, SIZE_MAX, &type});
    }

    RenderResult<StorageOwner>
    ResourceRegistry::create(ResourceCreateContext& dependencies, ResourceInstanceId id) noexcept
    {
        const auto type = entries_.find(id)->type;
        // A nested factory gets its own identity while sharing the caller's rollback list.
        auto construction = dependencies;
        construction.constructing_ = id;
        return type->factory.storage().create(construction, id, type->configuration.data());
    }

    void ResourceRegistry::publish(ResourceInstanceId id, std::size_t position, void* instance) noexcept
    {
        // A factory may have reentered another Set and grown the index. Reacquire after it returns.
        auto& entry = *entries_.find(id);
        entry.position = position;
        entry.instance = instance;
        ++creations_;
    }

    void ResourceRegistry::relocate(ResourceInstanceId id, std::size_t position) noexcept
    {
        entries_.find(id)->position = position;
    }

    void ResourceRegistry::unpublish(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance || !entry->graph_bindings.empty()) std::terminate();
        entry->instance = nullptr;
    }

    void ResourceRegistry::revoke(ResourceInstanceId id) noexcept
    {
        const auto* entry = entries_.find(id);
        // Removal qualified the incoming edges before unpublishing. Scope teardown qualifies them
        // once and destroys dependents first. A factory reservation was never available to require().
        if (!entry || entry->instance) std::terminate();
        entries_.erase(id);
    }

    std::optional<std::size_t> ResourceRegistry::position(ResourceInstanceId id, const ResourceSet& set) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance && entry->storage_set == &set ? std::optional{entry->position} : std::nullopt;
    }

    void* ResourceRegistry::resourceInstance(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->instance : nullptr;
    }

} // namespace lux::render
