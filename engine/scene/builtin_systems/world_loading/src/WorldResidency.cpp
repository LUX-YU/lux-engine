#include <lux/engine/scene/detail/WorldResidencyImpl.hpp>
#include <algorithm>
#include <unordered_set>

namespace lux::scene
{
    namespace ecs = simulation::ecs;
    using Ordinal = partition::PartitionOrdinal;
    PartitionRetention::PartitionRetention(std::shared_ptr<detail::PartitionRetentionRecord> record)
        : record_(std::move(record))
    {
        record_->uses.fetch_add(1, std::memory_order_relaxed);
    }
    PartitionRetention::PartitionRetention(PartitionRetention&&) noexcept = default;
    PartitionRetention& PartitionRetention::operator=(PartitionRetention&& other) noexcept
    {
        if (this != &other)
        {
            if (record_)
            {
                record_->uses.fetch_sub(1, std::memory_order_relaxed);
            }
            record_ = std::move(other.record_);
        }
        return *this;
    }
    PartitionRetention::~PartitionRetention()
    {
        if (record_)
        {
            record_->uses.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void WorldResidency::Impl::entityDestroyed(ecs::Entity entity) noexcept
    {
        // Component destruction can reinsert this generation into reactive
        // storage after Registry has already visited that storage. Remove it
        // at the final Entity signal, before this slot can be reused.
        identities.unbind(entity);
        // Keep loaded membership accurate when gameplay destroys an entity.
        // Unload first takes its list out, so this signal cannot invalidate its iteration.
        if (const auto found = membership.find(entity); found != membership.end())
        {
            if (auto* resident = find(found->second.partition))
            {
                std::erase(resident->entities, entity);
                resident->component_bytes -= found->second.component_bytes;
            }
            membership.erase(found);
        }
    }
    std::size_t WorldResidency::Impl::componentBytes(const ecs::Registry& registry, ecs::Entity entity) const
    {
        std::size_t bytes{};
        for (const auto& schema : schemas.all())
        {
            if (schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED &&
                schema.operations.has(registry, entity))
            {
                bytes += schema.operations.valueBytes();
            }
        }
        return bytes;
    }
    void WorldResidency::Impl::updateAccounting(
        ecs::Registry& registry,
        ecs::ComponentOperations::MembershipChanges& component_changes
    )
    {
        for (const auto [entity, changed] : component_changes.each())
        {
            const auto found = membership.find(entity);
            if (found != membership.end())
            {
                const auto bytes = componentBytes(registry, entity);
                auto& member = found->second;
                auto* resident = find(member.partition);
                resident->component_bytes -= member.component_bytes;
                resident->component_bytes += bytes;
                member.component_bytes = bytes;
            }
        }
        component_changes.clear();
    }
    std::size_t WorldResidency::Impl::unload(ecs::Registry& registry, const std::map<std::uint32_t, bool>& wanted)
    {
        if (residents.empty())
        {
            return 0;
        }
        bool has_candidate{};
        const bool unknown = std::ranges::any_of(residents, [](const auto& resident) { return resident.unknown; });
        for (std::size_t index{}; index < residents.size(); ++index)
        {
            auto& resident = residents[index];
            resident.retention = wanted.contains(resident.source->partition().value) ? EPartitionRetention::DEMAND
                                 : resident.dirty                                    ? EPartitionRetention::DIRTY
                                 : resident.protection->uses.load(std::memory_order_relaxed)
                                     ? EPartitionRetention::EXTERNAL
                                 : unknown ? EPartitionRetention::UNKNOWN_PAYLOAD
                                           : EPartitionRetention::NONE;
            has_candidate |= resident.retention == EPartitionRetention::NONE;
        }
        if (!has_candidate)
        {
            return 0;
        }

        std::vector<bool> keep(residents.size());
        for (std::size_t index{}; index < residents.size(); ++index)
            keep[index] = residents[index].retention != EPartitionRetention::NONE;

        // Compute retained-reference closure only when unloading is requested.
        // Full Entity generations identify currently resident targets; there is no
        // disk-wide WorldObjectId -> partition index and no implicit IO here.
        std::unordered_map<ecs::Entity, std::size_t> owner;
        for (std::size_t index{}; index < residents.size(); ++index)
        {
            for (const auto entity : residents[index].entities)
            {
                owner.emplace(entity, index);
            }
        }
        struct Edges final
        {
            const std::unordered_map<ecs::Entity, std::size_t>& owner;
            std::vector<std::pair<std::size_t, std::size_t>> values;
            std::size_t source{};
            static void visit(void* state, ecs::Entity entity) noexcept
            {
                auto& self = *static_cast<Edges*>(state);
                if (const auto found = self.owner.find(entity); found != self.owner.end())
                {
                    self.values.emplace_back(self.source, found->second);
                }
            }
        } edges{owner};
        const auto& pool = registry.storage<ecs::Entity>();
        for (std::size_t index{}; index < pool.free_list(); ++index)
        {
            const auto entity = pool.data()[index];
            const auto found = owner.find(entity);
            edges.source = found == owner.end() ? residents.size() : found->second;
            for (const auto& schema : schemas.all())
            {
                if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                    !schema.operations.has(registry, entity))
                {
                    continue;
                }
                if (!schema.visit_references || !schema.visit_references(registry, entity, {&edges, &Edges::visit}))
                {
                    for (auto& resident : residents)
                    {
                        resident.retention = EPartitionRetention::UNKNOWN_PAYLOAD;
                    }
                    return 0;
                }
            }
        }
        bool changed{};
        do
        {
            changed = false;
            for (const auto& [source, target] : edges.values)
            {
                if (!keep[target] && (source == residents.size() || keep[source]))
                {
                    keep[target] = true;
                    residents[target].retention = EPartitionRetention::REFERENCE;
                    changed = true;
                }
            }
        } while (changed);

        // Remove the complete unreferenced group at the owner's structural safe point.
        std::size_t unloaded{};
        for (std::size_t index = residents.size(); index-- > 0;)
        {
            if (keep[index])
            {
                continue;
            }
            const auto entities = std::move(residents[index].entities);
            for (const auto entity : entities)
            {
                identities.unbind(entity);
                if (registry.valid(entity))
                {
                    registry.destroy(entity);
                }
            }
            residents.erase(residents.begin() + static_cast<std::ptrdiff_t>(index));
            ++unloaded;
        }
        return unloaded;
    }
    WorldResidency::Impl::Resident* WorldResidency::Impl::find(std::uint32_t ordinal) noexcept
    {
        return const_cast<Resident*>(std::as_const(*this).find(ordinal));
    }
    const WorldResidency::Impl::Resident* WorldResidency::Impl::find(std::uint32_t ordinal) const noexcept
    {
        const auto found = std::ranges::find_if(residents, [ordinal](const auto& resident) {
            return resident.source->partition().value == ordinal;
        });
        return found == residents.end() ? nullptr : &*found;
    }
    WorldResidencyStatistics WorldResidency::Impl::statistics() const noexcept
    {
        WorldResidencyStatistics result;
        result.resident_partitions = residents.size();
        for (const auto& resident : residents)
        {
            result.resident_entities += resident.entities.size();
            result.resident_source_bytes += resident.source->retainedBytes();
            result.resident_component_bytes += resident.component_bytes;
        }
        return result;
    }
    WorldResidencyResult<void> WorldResidency::Impl::validateAdditional(std::size_t entities, std::size_t bytes)
        const noexcept
    {
        const auto used = statistics();
        const bool exceeds_entities = entities > limits.entities - std::min(limits.entities, used.resident_entities);
        const bool exceeds_bytes =
            bytes > limits.component_bytes - std::min(limits.component_bytes, used.resident_component_bytes);
        if (exceeds_entities || exceeds_bytes)
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::CAPACITY});
        return {};
    }
    WorldResidencyResult<void> WorldResidency::Impl::adopt(
        ecs::Registry& registry,
        std::span<const std::shared_ptr<const world::WorldPartitionData>> partitions
    )
    {
        const auto used = statistics();
        std::size_t source_bytes{}, objects{};
        std::vector<std::uint32_t> ordinals;
        for (const auto& source : partitions)
        {
            const bool invalid_source = !source || !world;
            if (invalid_source)
                return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::INVALID_PARTITION});
            const auto ordinal = source->partition();
            const bool wrong_version =
                source->bundle() != world->bundleId() || source->generation() != world->generation();
            const bool duplicate = find(ordinal.value) || std::ranges::find(ordinals, ordinal.value) != ordinals.end();
            if (wrong_version || duplicate || ordinal.value >= world->partitionCount())
                return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::INVALID_PARTITION, ordinal});
            if (source->retainedBytes() > limits.source_bytes - std::min(limits.source_bytes, source_bytes) ||
                source->objectCount() > limits.entities - std::min(limits.entities, objects))
                return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::CAPACITY, ordinal});
            source_bytes += source->retainedBytes();
            objects += source->objectCount();
            ordinals.push_back(ordinal.value);
        }
        if (partitions.size() > limits.partitions - std::min(limits.partitions, used.resident_partitions) ||
            source_bytes > limits.source_bytes - std::min(limits.source_bytes, used.resident_source_bytes) ||
            objects > limits.entities - std::min(limits.entities, used.resident_entities))
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::CAPACITY});
        std::vector<world::WorldPartitionObjectView> input;
        input.reserve(objects);
        for (const auto& source : partitions)
            for (std::size_t index{}; index < source->objectCount(); ++index)
                input.push_back(source->objectAt(index));
        auto materializer = WorldMaterializer::create(world, schemas);
        if (!materializer)
            return lux::cxx::unexpected(
                WorldResidencyFailure{EWorldResidencyError::MATERIALIZE_FAILURE, {}, materializer.error()}
            );
        std::vector<ecs::Entity> created;
        auto installed = materializer->objects(
            registry,
            identities,
            input,
            &created,
            limits.component_bytes - std::min(limits.component_bytes, used.resident_component_bytes)
        );
        if (!installed)
        {
            std::size_t first{};
            Ordinal ordinal{};
            for (const auto& source : partitions)
            {
                if (installed.error().object < first + source->objectCount())
                {
                    ordinal = source->partition();
                    break;
                }
                first += source->objectCount();
            }
            const auto code = installed.error().code == EWorldMaterializeError::CAPACITY
                                  ? EWorldResidencyError::CAPACITY
                                  : EWorldResidencyError::MATERIALIZE_FAILURE;
            return lux::cxx::unexpected(WorldResidencyFailure{code, ordinal, installed.error()});
        }
        std::size_t first{};
        for (const auto& source : partitions)
        {
            Resident resident;
            resident.source = source;
            resident.entities.assign(created.begin() + first, created.begin() + first + source->objectCount());
            for (std::size_t index{}; index < source->objectCount(); ++index)
            {
                const auto object = source->objectAt(index);
                for (std::size_t data{}; data < object.dataCount(); ++data)
                    resident.unknown |=
                        !schemas.find(ecs::componentSchemaId(world->schemas()[object.schemaOrdinalAt(data)].name));
            }
            for (const auto entity : resident.entities)
            {
                const auto bytes = componentBytes(registry, entity);
                membership.emplace(entity, Membership{source->partition().value, bytes});
                resident.component_bytes += bytes;
            }
            residents.push_back(std::move(resident));
            first += source->objectCount();
        }
        return {};
    }

    WorldResidency::WorldResidency(
        ecs::Registry& registry,
        std::shared_ptr<const world::WorldDescription> world,
        ecs::ComponentSchemaSet schemas,
        WorldResidencyLimits limits
    )
        : impl_(std::make_unique<Impl>(registry, std::move(world), std::move(schemas), limits))
    {}
    WorldResidency::~WorldResidency() noexcept = default;
    const world::WorldDescription& WorldResidency::description() const noexcept
    {
        return *impl_->world;
    }
    const ecs::WorldEntityMap& WorldResidency::identities() const noexcept
    {
        return impl_->identities;
    }
    WorldResidencyStatistics WorldResidency::statistics() const noexcept
    {
        return impl_->statistics();
    }
    WorldResidencyResult<void> WorldResidency::validateAdditional(std::size_t entities, std::size_t bytes)
        const noexcept
    {
        return impl_->validateAdditional(entities, bytes);
    }
    const world::WorldPartitionData* WorldResidency::source(Ordinal ordinal) const noexcept
    {
        const auto* resident = impl_->find(ordinal.value);
        return resident ? resident->source.get() : nullptr;
    }
    bool WorldResidency::Impl::partitionOf(ecs::Entity entity, Ordinal& partition) const noexcept
    {
        const auto found = membership.find(entity);
        if (found == membership.end())
            return false;
        partition = {found->second.partition};
        return true;
    }
    WorldResidencyResult<PartitionRetention> WorldResidency::Impl::retain(Ordinal ordinal)
    {
        const auto* resident = find(ordinal.value);
        if (!resident)
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::NOT_RESIDENT, ordinal});
        return PartitionRetention{resident->protection};
    }
    WorldResidencyResult<void> WorldResidency::Impl::setDirty(Ordinal ordinal, bool dirty)
    {
        auto* resident = find(ordinal.value);
        if (!resident)
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::NOT_RESIDENT, ordinal});
        resident->dirty = dirty;
        has_dirty |= dirty;
        return {};
    }
    void WorldResidency::Impl::clearDirty() noexcept
    {
        if (std::exchange(has_dirty, false))
            for (auto& resident : residents)
                resident.dirty = false;
    }
    std::vector<ResidentPartition> WorldResidency::Impl::resident() const
    {
        std::vector<ResidentPartition> result;
        result.reserve(residents.size());
        for (const auto& resident : residents)
            result.push_back(
                {resident.source->partition(),
                 resident.entities.size(),
                 resident.source->retainedBytes(),
                 resident.component_bytes,
                 resident.retention}
            );
        return result;
    }

    struct WorldResidency::PreparedChange::Impl final
    {
        struct Members final
        {
            Ordinal partition;
            std::vector<ecs::Entity> entities;
        };
        WorldResidency::Impl& residency;
        ecs::Registry& registry;
        std::optional<WorldMaterializer::PreparedObjects> creation;
        std::vector<ecs::Entity> removed;
        std::vector<Members> members;
        bool committed{};

        Impl(WorldResidency::Impl& owner, ecs::Registry& target) : residency(owner), registry(target) {}
        Members& partition(Ordinal ordinal)
        {
            auto found = std::ranges::find(members, ordinal, &Members::partition);
            if (found != members.end())
                return *found;
            members.push_back({ordinal, residency.find(ordinal.value)->entities});
            return members.back();
        }
        ecs::Entity entity(world::WorldObjectId id) const noexcept
        {
            const auto added = creation ? creation->entity(id) : ecs::NullEntity;
            if (added != ecs::NullEntity)
                return added;
            const auto existing = residency.identities.entity(id);
            return std::ranges::find(removed, existing) == removed.end() ? existing : ecs::NullEntity;
        }
        void commit() noexcept
        {
            if (std::exchange(committed, true))
                std::terminate();
            // The caller retains structural exclusion from prepare through commit.
            if (creation)
                creation->commit(registry, residency.identities);
            for (auto entity = removed.rbegin(); entity != removed.rend(); ++entity)
            {
                registry.destroy(*entity); // Observers still see the old identity during component destruction.
                residency.entityDestroyed(*entity);
            }
            for (auto& update : members)
            {
                auto* resident = residency.find(update.partition.value);
                for (const auto entity : resident->entities)
                    residency.membership.erase(entity);
                resident->entities = std::move(update.entities);
                resident->component_bytes = 0;
                for (const auto entity : resident->entities)
                {
                    const auto bytes = residency.componentBytes(registry, entity);
                    residency.membership.insert_or_assign(
                        entity,
                        WorldResidency::Impl::Membership{update.partition.value, bytes}
                    );
                    resident->component_bytes += bytes;
                }
                resident->dirty = true;
            }
            residency.has_dirty = true;
        }
    };

    WorldResidency::PreparedChange::PreparedChange(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    WorldResidency::PreparedChange::~PreparedChange() noexcept = default;
    WorldResidency::PreparedChange::PreparedChange(PreparedChange&&) noexcept = default;
    WorldResidency::PreparedChange& WorldResidency::PreparedChange::operator=(PreparedChange&&) noexcept = default;
    ecs::Entity WorldResidency::PreparedChange::entity(world::WorldObjectId id) const noexcept
    {
        return impl_->entity(id);
    }
    std::span<const ecs::Entity> WorldResidency::PreparedChange::references() const noexcept
    {
        return impl_->creation ? impl_->creation->references() : std::span<const ecs::Entity>{};
    }
    std::size_t WorldResidency::PreparedChange::retainedBytes() const noexcept
    {
        std::size_t bytes = sizeof(Impl) + impl_->removed.capacity() * sizeof(ecs::Entity) +
                            impl_->members.capacity() * sizeof(Impl::Members);
        if (impl_->creation)
            bytes += impl_->creation->componentBytes();
        for (const auto& members : impl_->members)
            bytes += members.entities.capacity() * sizeof(ecs::Entity);
        return bytes;
    }
    void WorldResidency::PreparedChange::commit() noexcept
    {
        impl_->commit();
    }

    WorldResidencyResult<WorldResidency::PreparedChange> WorldResidency::Impl::prepareCreate(
        ecs::Registry& registry,
        std::span<const ObjectInput> input
    ) noexcept
    {
        if (&registry != owner_registry)
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::INVALID_OBJECT});
        const auto capacity = validateAdditional(input.size(), 0);
        if (!capacity)
            return lux::cxx::unexpected(capacity.error());
        std::vector<WorldObjectInput> objects;
        objects.reserve(input.size());
        for (const auto& object : input)
        {
            // Unlike adopting a stored partition, this edit has no opaque source owner.
            for (const auto& component : object.object.components)
                if (!component.schema)
                    return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::INVALID_OBJECT});
            if (!find(object.partition.value))
                return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::NOT_RESIDENT, object.partition}
                );
            objects.push_back(object.object);
        }
        const auto used = statistics();
        auto prepared = WorldMaterializer::prepareObjects(
            registry,
            identities,
            objects,
            limits.component_bytes - std::min(limits.component_bytes, used.resident_component_bytes)
        );
        if (!prepared)
        {
            const auto code = prepared.error().code == EWorldMaterializeError::CAPACITY
                                  ? EWorldResidencyError::CAPACITY
                                  : EWorldResidencyError::MATERIALIZE_FAILURE;
            const auto partition =
                prepared.error().object < input.size() ? input[prepared.error().object].partition : Ordinal{};
            return lux::cxx::unexpected(WorldResidencyFailure{code, partition, prepared.error()});
        }
        auto change = std::make_unique<PreparedChange::Impl>(*this, registry);
        change->creation.emplace(std::move(*prepared));
        for (std::size_t index{}; index < input.size(); ++index)
            change->partition(input[index].partition).entities.push_back(change->creation->entities()[index]);
        return PreparedChange{std::move(change)};
    }

    WorldResidencyResult<WorldResidency::PreparedChange> WorldResidency::Impl::prepareErase(
        ecs::Registry& registry,
        std::span<const world::WorldObjectId> input
    ) noexcept
    {
        if (&registry != owner_registry)
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::INVALID_OBJECT});
        auto change = std::make_unique<PreparedChange::Impl>(*this, registry);
        std::unordered_set<ecs::Entity> removed;
        for (const auto id : input)
        {
            const auto entity = identities.entity(id);
            const auto member = membership.find(entity);
            const bool valid = registry.valid(entity) && member != membership.end();
            if (!valid || !removed.insert(entity).second)
                return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::INVALID_OBJECT});
            std::erase(change->partition({member->second.partition}).entities, entity);
            change->removed.push_back(entity);
        }
        const bool has_unknown = std::ranges::any_of(residents, [](const auto& resident) { return resident.unknown; });
        if (has_unknown)
        {
            // Opaque source payload can refer to original IDs, but cannot refer to newly authored IDs.
            for (const auto& resident : residents)
                for (std::size_t index{}; index < resident.source->objectCount(); ++index)
                    if (removed.contains(identities.entity(resident.source->objectAt(index).id())))
                        return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::REFERENCE_IN_USE});
        }
        struct References final
        {
            const std::unordered_set<ecs::Entity>& removed;
            bool unsafe{};
            static void visit(void* state, ecs::Entity target) noexcept
            {
                auto& value = *static_cast<References*>(state);
                value.unsafe |= value.removed.contains(target);
            }
        } references{removed};
        const auto& entities = registry.storage<ecs::Entity>();
        for (std::size_t index{}; index < entities.free_list(); ++index)
        {
            const auto entity = entities.data()[index];
            if (removed.contains(entity))
                continue;
            for (const auto& schema : schemas.all())
            {
                if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                    !schema.operations.has(registry, entity))
                    continue;
                if (!schema.visit_references ||
                    !schema.visit_references(registry, entity, {&references, &References::visit}))
                    references.unsafe = true;
            }
        }
        if (references.unsafe)
            return lux::cxx::unexpected(WorldResidencyFailure{EWorldResidencyError::REFERENCE_IN_USE});
        return PreparedChange{std::move(change)};
    }
    WorldResidencyResult<WorldResidency::PreparedChange> WorldResidency::prepareCreate(
        ecs::Registry& registry,
        std::span<const ObjectInput> input
    ) noexcept
    {
        return impl_->prepareCreate(registry, input);
    }
    WorldResidencyResult<WorldResidency::PreparedChange> WorldResidency::prepareErase(
        ecs::Registry& registry,
        std::span<const world::WorldObjectId> input
    ) noexcept
    {
        return impl_->prepareErase(registry, input);
    }
    bool WorldResidency::partitionOf(ecs::Entity entity, Ordinal& partition) const noexcept
    {
        return impl_->partitionOf(entity, partition);
    }
    WorldResidencyResult<PartitionRetention> WorldResidency::retain(Ordinal partition)
    {
        return impl_->retain(partition);
    }
    WorldResidencyResult<void> WorldResidency::setDirty(Ordinal partition, bool dirty)
    {
        return impl_->setDirty(partition, dirty);
    }
    void WorldResidency::clearDirty() noexcept
    {
        impl_->clearDirty();
    }
    std::vector<ResidentPartition> WorldResidency::resident() const
    {
        return impl_->resident();
    }
}
