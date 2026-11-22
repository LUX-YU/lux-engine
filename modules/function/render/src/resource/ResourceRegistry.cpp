#include <algorithm>
#include <lux/engine/render/resource/GraphInstallation.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <exception>
#include <limits>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
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

    RenderResult<void> ResourceRegistry::qualifyBufferUses(
        const ProjectionMap& projections,
        std::uint32_t projection,
        const std::array<ProjectionBuffer, 3>& buffers,
        const GraphBindingUse& user
    ) noexcept
    {
        const auto overlaps = [](GraphBufferRange a, GraphBufferRange b) noexcept
        {
            return a.offset <= b.offset ? b.offset - a.offset < a.size : a.offset - b.offset < b.size;
        };
        const auto physical = [](ProjectionBuffer slice, std::optional<GraphBufferRange> writer) noexcept
        {
            if (writer) return GraphBufferRange{slice.bytes.offset + writer->offset, writer->size};
            return slice.bytes;
        };
        for (const auto& slice : std::span{buffers}.first(user.instance->frameCount()))
        {
            if (slice.bytes.size > UINT64_MAX - slice.bytes.offset)
                return cxx::unexpected(RenderError{kGraphInvalidRange});
            if (!user.writer) continue;
            const auto claim = *user.writer;
            if (!slice.buffer || claim.offset > slice.bytes.size || claim.size > slice.bytes.size - claim.offset)
                return cxx::unexpected(RenderError{kGraphInvalidRange});
        }
        for (const auto& [other_projection, binding] : projections)
        {
            const bool same_projection = projection == other_projection;
            const auto& previous = same_projection ? buffers : binding.backing.buffers;
            for (const auto& bound : binding.users)
            {
                if (!bound.writer && !user.writer) continue;
                const bool replacing = bound.job == user.job && bound.generation != user.generation;
                const bool same_use = bound.job == user.job && bound.resource == user.resource;
                // Disabled command bodies still record their planned barriers.
                const bool compatible_barriers = same_projection && bound.canonical_queue != kGraphInvalidIndex &&
                    bound.canonical_queue == user.canonical_queue;
                const bool exclusive_paths = compatible_barriers && bound.job == user.job && bound.writer &&
                    user.writer && (!bound.may_access || !user.may_access);
                if (replacing || same_use || exclusive_paths) continue;
                const auto conflict = bound.job == user.job ? kGraphConflictingOutput : kJobBusy;
                if (same_projection && (!bound.writer || !user.writer || overlaps(*bound.writer, *user.writer)))
                    return cxx::unexpected(RenderError{conflict});
                for (const auto& a : buffers)
                {
                    if (!a.buffer) continue;
                    for (const auto& b : previous)
                        if (a.buffer == b.buffer && overlaps(physical(a, user.writer), physical(b, bound.writer)))
                            return cxx::unexpected(RenderError{conflict});
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
        if (auto valid = qualifyBufferUses(projections, projection, backing.buffers, user); !valid) return valid;
        const auto existing = projections.find(projection);
        if (existing != projections.end() && existing->second.backing != backing)
            return cxx::unexpected(RenderError{kStorageMismatch});
        return {};
    }

    RenderResult<ResourceRegistry::ProjectionFamily>
    ResourceRegistry::resolveGraphBacking(
        const runtime_detail::BoundStorageProjection& link, const GraphBindingUse& user
    ) noexcept
    {
        auto& instance = *user.instance;
        auto* entry = entries_.find(link.storage);
        if (!entry || !entry->execution_uses) return cxx::unexpected(RenderError{kStorageIdentity});
        const auto installed = entry->graph_bindings.find(link.projection);
        if (installed != entry->graph_bindings.end())
        {
            if (auto valid = qualifyBufferUses(entry->graph_bindings, link.projection, installed->second.backing.buffers, user); !valid)
                return cxx::unexpected(valid.error());
        }
        const auto& ops = *types_.find(entry->type)->factory.storage()->bindings();
        std::array<detail::ResourceInput, 3> slices{};
        auto output = std::span(slices).first(instance.frameCount());
        const auto& description = instance.plan().resources()[link.resource.index].description;
        if (auto resolved = ops.resolveGraphImport(entry->instance, link.projection, description, output); !resolved)
            return cxx::unexpected(resolved.error());

        // An extension may have created other resources during resolution. Reacquire the row by identity.
        entry = entries_.find(link.storage);
        ProjectionFamily candidate;
        auto& buffers = candidate.buffers;
        auto& images = candidate.images;
        auto& queue_uses = candidate.queue_uses;
        const bool is_image = std::holds_alternative<GraphImageDesc>(description);
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
                const auto& compiled = instance.plan().resources()[link.resource.index];
                if (compiled.concurrent_image_read && (!queue_uses[i].preparation || !queue_uses[i].accepted))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                if (auto valid = instance.qualifyImportedImage(link.resource, *images[i], instance.target()); !valid)
                    return cxx::unexpected(valid.error());
            }
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
            instance.setResource(link.resource.index, slot, output[slot]);
        return candidate;
    }

    void ResourceRegistry::unbindGraphProjection(
        const runtime_detail::BoundStorageProjection& link, const detail::ExecutionBindingId& identity
    ) noexcept
    {
        auto& projections = entries_.find(link.storage)->graph_bindings;
        const auto found = projections.find(link.projection);
        if (found == projections.end()) std::terminate();
        auto& users = found->second.users;
        const auto* reader = users.find(link.subscription);
        const bool current = reader && reader->job == identity.job && reader->generation == identity.generation;
        if (!current || reader->resource != link.resource) std::terminate();
        if (link.subscription.gen == std::numeric_limits<decltype(link.subscription.gen)>::max() - 1)
            ++found->second.retired_slots;
        users.erase(link.subscription);
        if (users.empty()) projections.erase(found);
        if (link.output_range) pruneBufferProducts(link.storage);
    }

    RenderResult<std::size_t> ResourceRegistry::publishGraphInput(
        ResourceInstanceId storage, std::uint32_t projection, std::uint32_t frame, const detail::ResourceInput& value
    ) noexcept
    {
        auto* entry = entries_.find(storage);
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        // One Storage can expose the same backing through multiple projections.
        if (installation_ && installation_->holds(storage)) return cxx::unexpected(RenderError{kDeviceBusy});
        const auto found = entry->graph_bindings.find(projection);
        const auto publish_image = [&]() -> RenderResult<std::size_t>
        {
            if (hasBufferFields(value)) return cxx::unexpected(RenderError{kStorageMismatch});
            const GraphImagePublication update{projection, frame, value.image, value.queue_uses};
            return publishGraphImages(storage, {&update, 1});
        };
        if (value.image) return publish_image();
        if (found != entry->graph_bindings.end())
        {
            const auto& user = *found->second.users.begin();
            const auto& desc = user.instance->plan().resources()[user.resource.index].description;
            if (std::holds_alternative<GraphImageDesc>(desc)) return publish_image();
        }
        if (!validQueueUses(value.buffer != nullptr, value.queue_uses, *deviceOf(storage)))
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (value.size > UINT64_MAX - value.offset) return cxx::unexpected(RenderError{kGraphInvalidRange});
        if (found == entry->graph_bindings.end())
        {
            if (entry->products.empty()) return 0;
            std::array<ProjectionBuffer, 3> replacement;
            replacement.fill({value.buffer, {value.offset, value.size}});
            if (frame != UINT32_MAX && frame >= replacement.size()) return cxx::unexpected(RenderError{kFrameContract});
            const auto first = frame == UINT32_MAX ? 0u : frame;
            auto slices = std::span{replacement}.first(frame == UINT32_MAX ? replacement.size() : 1);
            if (auto valid = replaceProductBacking(storage, projection, first, slices); !valid)
                return cxx::unexpected(valid.error());
            pruneBufferProducts(storage);
            return 0;
        }
        auto& binding = found->second;
        const auto& users = binding.users;
        // Each storage belongs to one device with a single configured FIF count.
        const auto count = users.begin()->instance->frameCount();
        if (frame != UINT32_MAX && frame >= count)
            return cxx::unexpected(RenderError{kFrameContract});
        auto candidate = binding.backing.buffers;
        const auto first = frame == UINT32_MAX ? 0u : frame;
        const auto end = frame == UINT32_MAX ? count : frame + 1;
        for (auto i = first; i < end; ++i) candidate[i] = {value.buffer, {value.offset, value.size}};
        if (candidate != binding.backing.buffers)
        {
            const bool has_writer = std::ranges::any_of(entry->graph_bindings, [](const auto& current)
            {
                return std::ranges::any_of(current.second.users, [](const auto& use)
                {
                    return use.writer.has_value();
                });
            });
            if (has_writer)
            {
                for (const auto& user : users)
                    if (auto valid = qualifyBufferUses(entry->graph_bindings, projection, candidate, user); !valid)
                        return cxx::unexpected(valid.error());
            }
            const auto slices = std::span{candidate}.subspan(first, end - first);
            if (auto valid = replaceProductBacking(storage, projection, first, slices); !valid)
                return cxx::unexpected(valid.error());
            binding.backing.buffers = candidate;
            if (!entry->product_buffers.empty()) pruneBufferProducts(storage);
        }
        for (auto i = first; i < end; ++i) binding.backing.queue_uses[i] = value.queue_uses;
        for (const auto& user : users) user.instance->setResource(user.resource.index, frame, value);
        return users.size();
    }

    RenderResult<std::size_t>
    ResourceRegistry::publishGraphImages(
        ResourceInstanceId storage, std::span<const GraphImagePublication> updates
    ) noexcept
    {
        auto* entry = entries_.find(storage);
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        if (!updates.empty() && installation_ && installation_->holds(storage))
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
            const auto count = family.users.begin()->instance->frameCount();
            if (update.frame != UINT32_MAX && update.frame >= count)
                return cxx::unexpected(RenderError{kFrameContract});
            for (const auto& user : family.users)
            {
                const auto& graph_resource = user.instance->plan().resources()[user.resource.index];
                if (!std::holds_alternative<GraphImageDesc>(graph_resource.description))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                for (std::uint32_t f = 0; f < count; ++f)
                {
                    if (!includesFrame(update, f)) continue;
                    if (family.backing.images[f] == update.image && family.backing.queue_uses[f] == update.uses) continue;
                    if (user.instance->frozenForSubmission(f)) return cxx::unexpected(RenderError{kDeviceBusy});
                }
                if (update.image)
                {
                    if (graph_resource.concurrent_image_read && (!update.uses.preparation || !update.uses.accepted))
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    const auto& reader = *user.instance;
                    auto valid = reader.qualifyImportedImage(user.resource, *update.image, reader.target());
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
            const auto count = family.users.begin()->instance->frameCount();
            for (std::uint32_t f = 0; f < count; ++f)
            {
                if (!includesFrame(update, f)) continue;
                if (family.backing.images[f] == update.image && family.backing.queue_uses[f] == update.uses) continue;
                family.backing.images[f] = update.image;
                family.backing.queue_uses[f] = update.uses;
                const detail::ResourceInput input{.image = update.image, .queue_uses = update.uses};
                for (const auto& user : family.users) user.instance->setResource(user.resource.index, f, input);
            }
            updated += family.users.size();
        }
        return updated;
    }

    void runtime_detail::finishResourceCommand(ResourceRegistry& registry, ResourceInstanceId id) noexcept
    {
        if (!id.isNull()) registry.finishCommand(id);
    }

    RenderResult<ResourceRegistry::ResolvedCommand>
    ResourceRegistry::beginCommand(ResourceInstanceId id, CommandId command, bool exclusive) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& definition = *types_.find(entry->type);
        const auto location = std::ranges::find(definition.commands, command, &CommandBindingLocation::command);
        if (location == definition.commands.end())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const bool conflicting_command = entry->exclusive_command || (exclusive && entry->command_uses != 0);
        if (conflicting_command)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        if (entry->command_uses == UINT32_MAX) std::terminate();
        ++entry->command_uses;
        entry->exclusive_command = exclusive;
        return ResolvedCommand{*location, definition.factory.storage()->implementation_type};
    }

    bool ResourceRegistry::hasBorrows(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && (entry->command_uses != 0 || entry->execution_uses != 0);
    }

    bool ResourceRegistry::hasDependents(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->dependent_storages != 0;
    }

    RenderResult<ResourceInstanceId> ResourceRegistry::requireDuringConstruction(
        ResourceCreateContext& context, ResourceInstanceId consumer, ResourceTypeId type
    ) noexcept
    {
        auto* entry = entries_.find(consumer);
        const auto* definition = factory(type);
        if (!entry || entry->instance || !definition) return cxx::unexpected(RenderError{kStorageContract});
        auto* scope = entry->storage_set->ownerFor(definition->descriptor->domain);
        if (!scope) return cxx::unexpected(RenderError{kStorageMismatch});
        auto dependency = scope->getOrCreate(context, type);
        if (!dependency) return cxx::unexpected(dependency.error());

        auto& dependencies = entries_.find(consumer)->dependencies;
        if (std::ranges::find(dependencies, *dependency) == dependencies.end())
        {
            auto& count = entries_.find(*dependency)->dependent_storages;
            if (count == UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
            dependencies.push_back(*dependency);
            ++count;
        }
        return *dependency;
    }

    RenderResult<void> ResourceRegistry::retainExecution(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance) return cxx::unexpected(RenderError{kStorageIdentity});
        if (entry->exclusive_command) return cxx::unexpected(RenderError{kDeviceBusy});
        if (entry->execution_uses == UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        ++entry->execution_uses;
        return {};
    }

    void ResourceRegistry::releaseExecution(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->execution_uses) std::terminate();
        --entry->execution_uses;
    }

    bool ResourceRegistry::hasExclusiveCommand(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->exclusive_command;
    }

    const detail::DeviceSession* ResourceRegistry::deviceOf(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? &entry->storage_set->device_ : nullptr;
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
        return entry && entry->instance ? entry->type : ResourceTypeId{};
    }

    const CommandBindingLocation*
    ResourceRegistry::commandBinding(ResourceTypeId type, CommandId command) const noexcept
    {
        const auto* definition = types_.find(type);
        if (!definition)
        {
            return nullptr;
        }
        const auto found = std::ranges::find(definition->commands, command, &CommandBindingLocation::command);
        return found == definition->commands.end() ? nullptr : &*found;
    }

    RenderResult<void> ResourceRegistry::bindCommand(ResourceTypeId type, CommandBindingLocation location) noexcept
    {
        auto* definition = types_.find(type);
        if (!definition || location.command.isNull())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (const auto* previous = commandBinding(type, location.command))
        {
            if (*previous != location)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return {};
        }
        definition->commands.push_back(location);
        return {};
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

    const ConfigValue* ResourceRegistry::configuration(ResourceTypeId type) const noexcept
    {
        const auto* entry = types_.find(type);
        return entry ? &entry->configuration : nullptr;
    }

    RenderResult<const ResourceRegistration*>
    ResourceRegistry::qualify(ResourceTypeId type, EResourceDomain domain, const ResourceCreateContext& context)
        const noexcept
    {
        const auto* definition = types_.find(type);
        if (!definition)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        const bool is_wrong_registry = &context.resources != this;
        const bool is_wrong_domain = definition->factory.descriptor->domain != domain;
        if (is_wrong_registry || is_wrong_domain)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (!definition->factory.descriptor->supports(context.device.resourceCapabilities()))
        {
            return cxx::unexpected(RenderError{kStorageCapability});
        }
        if (definition->factory.storage()->config_type.isValid() && !definition->configuration)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        return &definition->factory;
    }

    RenderResult<void> ResourceRegistry::validateFactory(const ResourceRegistration& factory) const noexcept
    {
        if (!factory.descriptor || !factory.storage())
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto valid = validateResourceRegistration(factory); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (const auto* ops = factory.storage()->bindings())
        {
            const bool has_prepare = ops->prepareFrame != nullptr;
            const bool has_settle = ops->settleFrame != nullptr;
            if (has_prepare != has_settle) return cxx::unexpected(RenderError{kStorageContract});
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
        const bool is_missing_config = factory.storage()->config_type.isValid() && !configuration;
        if (is_missing_config) return cxx::unexpected(RenderError{kStorageContract});
        const bool is_wrong_config_type = configuration && factory.storage()->config_type != configuration.type();
        if (is_wrong_config_type)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    ResourceRegistry::Type::Type(ResourceRegistration source, ConfigValue config) noexcept
        : descriptor(*source.descriptor), frame_inputs(descriptor.frame_inputs.begin(), descriptor.frame_inputs.end()),
          frame_channels(source.frame_call.channels.begin(), source.frame_call.channels.end()),
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

        command_descriptors.reserve(factory.commands.size());
        command_views.reserve(factory.commands.size());
        command_bindings.reserve(factory.commands.size());
        for (std::size_t i = 0; i < factory.commands.size(); ++i)
        {
            auto& declaration = command_descriptors.emplace_back(*descriptor.commands[i]);
            declaration.name = CommandName{own_name(declaration.name.name())};
            own_type(declaration.request);
            own_type(declaration.response);
            own_type(declaration.data);
            command_views.push_back(&declaration);

            auto& binding = command_bindings.emplace_back(factory.commands[i]);
            binding.descriptor_ = &declaration;
            own_type(binding.storage_type_);
            own_type(binding.method_type_);
        }
        descriptor.commands = command_views;
        factory.commands = command_bindings;

        auto& call = factory.frame_call;
        own_type(call.declaration_type);
        own_type(call.storage_type);
        own_type(call.method_type);
        for (auto& channel : frame_channels)
        {
            own_type(channel.declaration);
            own_type(channel.element);
        }
        call.channels = frame_channels;

        auto& storage = *factory.storage_;
        own_type(storage.config_type);
        own_type(storage.ops_type);
        own_type(storage.implementation_type);
        if (storage.snapshot_ops) own_type(storage.snapshot_ops->type);
    }

    ResourceTypeId ResourceRegistry::publishFactory(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        const auto id = types_.emplace(std::move(factory), std::move(configuration));
        types_by_name_.emplace(std::string(types_.find(id)->factory.descriptor->type.name()), id);
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

    ResourceInstanceId ResourceRegistry::reserve(ResourceSet& set, ResourceTypeId type) noexcept
    {
        return entries_.emplace(Entry{&set, 0, type});
    }

    RenderResult<StorageOwner>
    ResourceRegistry::create(
        ResourceCreateContext& dependencies,
        ResourceInstanceId id,
        const void* configuration
    ) noexcept
    {
        const auto type = entries_.find(id)->type;
        return types_.find(type)->factory.storage()->create(dependencies, id, configuration);
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

    void ResourceRegistry::revoke(ResourceInstanceId id) noexcept
    {
        if (hasBorrows(id) || hasDependents(id) || !entries_.find(id)->graph_bindings.empty())
        {
            std::terminate();
        }
        for (auto dependency : entries_.find(id)->dependencies)
        {
            auto* owner = entries_.find(dependency);
            if (!owner || !owner->dependent_storages) std::terminate();
            --owner->dependent_storages;
        }
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
