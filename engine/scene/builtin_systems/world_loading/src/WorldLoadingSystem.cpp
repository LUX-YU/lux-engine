#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <unordered_map>

namespace lux::scene
{
namespace ecs = simulation::ecs;
namespace loading = process::world_loading;
using Ordinal = partition::PartitionOrdinal;

namespace detail
{
struct PartitionRetentionRecord final
{
    std::atomic<std::size_t> uses{};
};
} // namespace detail

PartitionRetention::PartitionRetention(std::shared_ptr<detail::PartitionRetentionRecord> record)
    : record_(std::move(record))
{
    record_->uses.fetch_add(1, std::memory_order_relaxed);
}
PartitionRetention::PartitionRetention(PartitionRetention &&) noexcept = default;
PartitionRetention &PartitionRetention::operator=(PartitionRetention &&other) noexcept
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

struct WorldLoadingSystem::Impl final
{
    struct Read final
    {
        enum class State : std::uint8_t
        {
            PENDING,
            VALUE,
            ERROR,
            CANCELLED
        };
        std::atomic<State> state{State::PENDING};
        std::shared_ptr<const world::WorldPartitionData> data;
        loading::WorldStorageRuntimeFailure error;
        std::stop_source cancel;
        Ordinal partition;
        std::uint64_t source_epoch{};
        bool obsolete{}; // Owner-thread qualification, never accessed by completion.
    };
    struct Resident final
    {
        std::shared_ptr<const world::WorldPartitionData> source;
        std::vector<ecs::Entity> entities;
        std::shared_ptr<detail::PartitionRetentionRecord> protection{
            std::make_shared<detail::PartitionRetentionRecord>()};
        std::size_t component_bytes{};
        bool dirty{}, unknown{};
        EPartitionRetention retention{};
    };
    struct Membership final
    {
        std::uint32_t partition;
        std::size_t component_bytes;
    };

    ecs::Registry &registry;
    ecs::ComponentSchemaSet schemas;
    WorldLoadingServices services;
    ecs::WorldEntityMap identities;
    std::map<std::uint32_t, bool> wanted; // Union: true if at least one demand is required.
    std::vector<std::shared_ptr<Read>> reads;
    std::vector<Resident> residents;
    std::unordered_map<ecs::Entity, Membership> membership;
    ecs::ComponentOperations::MembershipChanges &component_changes;
    entt::scoped_connection construct, update, destroy, entity_destroy;
    WorldLoadingResult<void> result;
    WorldLoadingStatistics stats;
    std::uint64_t source_epoch{1};
    bool demand_dirty{true}, stopping{}, has_dirty{};

    Impl(ecs::Registry &registry, ecs::ComponentSchemaSet schemas, WorldLoadingServices services)
        : registry(registry), schemas(std::move(schemas)), services(std::move(services)),
          component_changes(registry.storage<entt::reactive>(entt::hashed_string{"scene.loading.membership"}.value())),
          construct(registry.on_construct<Observer>().connect<&Impl::changed>(*this)),
          update(registry.on_update<Observer>().connect<&Impl::changed>(*this)),
          destroy(registry.on_destroy<Observer>().connect<&Impl::changed>(*this)),
          entity_destroy(registry.on_destroy<ecs::Entity>().connect<&Impl::entityDestroyed>(*this))
    {
        for (const auto &schema : this->schemas.all())
        {
            if (schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED)
            {
                schema.operations.trackMembership(component_changes);
            }
        }
        const auto &limits = this->services.limits;
        if (!this->services.source || !limits.in_flight || !limits.partitions || !limits.entities ||
            !limits.read_bytes || limits.read_bytes > limits.staging_bytes || !limits.requests_per_turn)
        {
            result = lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_CONFIGURATION});
        }
    }

    ~Impl()
    {
        component_changes.reset();
        component_changes.clear();
        construct.release();
        update.release();
        destroy.release();
        entity_destroy.release();
        for (const auto &read : reads)
        {
            read->cancel.request_stop();
        }
        // Read senders retain source, operation/result and provider code. No callback
        // borrows this instance. Registry owns installed components until Scene exit.
    }
    void changed(ecs::Registry &, ecs::Entity) noexcept
    {
        demand_dirty = true;
    }
    void entityDestroyed(ecs::Registry &, ecs::Entity entity) noexcept
    {
        // Component destruction can reinsert this generation into reactive
        // storage after Registry has already visited that storage. Remove it
        // at the final Entity signal, before this slot can be reused.
        component_changes.remove(entity);
        identities.unbind(entity);
        // Keep loaded membership accurate when gameplay destroys an entity.
        // Unload first takes its list out, so this signal cannot invalidate its iteration.
        if (const auto found = membership.find(entity); found != membership.end())
        {
            if (auto *resident = find(found->second.partition))
            {
                std::erase(resident->entities, entity);
                resident->component_bytes -= found->second.component_bytes;
            }
            membership.erase(found);
        }
    }

    std::size_t componentBytes(ecs::Entity entity) const
    {
        std::size_t bytes{};
        for (const auto &schema : schemas.all())
        {
            if (schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED &&
                schema.operations.has(registry, entity))
            {
                bytes += schema.operations.valueBytes();
            }
        }
        return bytes;
    }

    void updateAccounting()
    {
        for (const auto [entity, changed] : component_changes.each())
        {
            const auto found = membership.find(entity);
            if (found != membership.end())
            {
                const auto bytes = componentBytes(entity);
                auto &member = found->second;
                auto *resident = find(member.partition);
                resident->component_bytes -= member.component_bytes;
                resident->component_bytes += bytes;
                member.component_bytes = bytes;
            }
        }
        component_changes.clear();
    }

    Resident *find(std::uint32_t ordinal)
    {
        const auto found = std::ranges::find_if(
            residents, [ordinal](const auto &resident) { return resident.source->partition().value == ordinal; });
        return found == residents.end() ? nullptr : &*found;
    }
    void fail(WorldLoadingFailure failure)
    {
        if (result)
        {
            result = lux::cxx::unexpected(std::move(failure));
        }
    }
    void recount()
    {
        stats.in_flight = stats.reserved_read_bytes = stats.staged_bytes = 0;
        for (const auto &read : reads)
        {
            if (read->state.load(std::memory_order_acquire) == Read::State::PENDING)
            {
                ++stats.in_flight;
                stats.reserved_read_bytes += services.limits.read_bytes;
            }
            else if (read->state.load(std::memory_order_acquire) == Read::State::VALUE)
            {
                stats.staged_bytes += read->data->retainedBytes();
            }
        }
        stats.resident_partitions = residents.size();
        stats.resident_entities = stats.resident_source_bytes = stats.resident_component_bytes = 0;
        for (const auto &resident : residents)
        {
            stats.resident_entities += resident.entities.size();
            stats.resident_source_bytes += resident.source->retainedBytes();
            stats.resident_component_bytes += resident.component_bytes;
        }
    }
    bool requiredMissing() const
    {
        for (const auto &[ordinal, required] : wanted)
        {
            if (required && std::ranges::none_of(residents, [ordinal](const auto &resident) {
                    return resident.source->partition().value == ordinal;
                }))
            {
                return true;
            }
        }
        return false;
    }
    SceneStageResult progress() const
    {
        const bool missing = requiredMissing();
        if (!result && result.error().code != EWorldLoadingError::CAPACITY &&
            (missing || result.error().code == EWorldLoadingError::INVALID_PARTITION))
        {
            return lux::cxx::unexpected(
                SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, result.error()});
        }
        return missing ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
    }
    void refreshDemands()
    {
        if (!demand_dirty)
        {
            return;
        }
        demand_dirty = false;
        std::map<std::uint32_t, bool> next;
        WorldLoadingResult<void> validation;
        auto add = [&](Ordinal partition, bool required) {
            if (partition.value >= services.source.world().partitionCount())
            {
                validation =
                    lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_PARTITION, partition});
                return;
            }
            next[partition.value] |= required;
        };
        for (const auto partition : services.bootstrap)
        {
            add(partition, true);
        }
        for (const auto entity : registry.view<const Observer>())
        {
            const auto &observer = registry.get<Observer>(entity);
            for (const auto partition : observer.partitions)
            {
                add(partition, observer.required);
            }
        }
        if (!validation)
        {
            fail(validation.error());
            return;
        }
        if (!result && result.error().code == EWorldLoadingError::INVALID_PARTITION)
        {
            result = {};
        }
        if (next == wanted)
        {
            return;
        }
        wanted = std::move(next);
        ++stats.demand_revision;
        // A new demand revision does not invalidate a still-needed, same-source read.
        if (!result && result.error().code != EWorldLoadingError::INVALID_CONFIGURATION)
        {
            result = {};
        }
        for (const auto &read : reads)
        {
            if (!wanted.contains(read->partition.value))
            {
                read->obsolete = true;
                read->cancel.request_stop();
            }
        }
    }

    void unload(SceneStageContext &context)
    {
        if (!context.allow_structure || residents.empty())
        {
            return;
        }
        std::vector<bool> keep(residents.size());
        bool has_candidate{};
        const bool unknown = std::ranges::any_of(residents, [](const auto &resident) { return resident.unknown; });
        for (std::size_t index{}; index < residents.size(); ++index)
        {
            auto &resident = residents[index];
            resident.retention = wanted.contains(resident.source->partition().value) ? EPartitionRetention::DEMAND
                                 : resident.dirty                                    ? EPartitionRetention::DIRTY
                                 : resident.protection->uses.load(std::memory_order_relaxed)
                                     ? EPartitionRetention::EXTERNAL
                                 : unknown ? EPartitionRetention::UNKNOWN_PAYLOAD
                                           : EPartitionRetention::NONE;
            keep[index] = resident.retention != EPartitionRetention::NONE;
            has_candidate |= !keep[index];
        }
        if (!has_candidate)
        {
            return;
        }

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
            const std::unordered_map<ecs::Entity, std::size_t> &owner;
            std::vector<std::pair<std::size_t, std::size_t>> values;
            std::size_t source{};
            static void visit(void *state, ecs::Entity entity) noexcept
            {
                auto &self = *static_cast<Edges *>(state);
                if (const auto found = self.owner.find(entity); found != self.owner.end())
                {
                    self.values.emplace_back(self.source, found->second);
                }
            }
        } edges{owner};
        const auto &pool = registry.storage<ecs::Entity>();
        for (std::size_t index{}; index < pool.free_list(); ++index)
        {
            const auto entity = pool.data()[index];
            const auto found = owner.find(entity);
            edges.source = found == owner.end() ? residents.size() : found->second;
            for (const auto &schema : schemas.all())
            {
                if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                    !schema.operations.has(registry, entity))
                {
                    continue;
                }
                if (!schema.visit_references || !schema.visit_references(registry, entity, {&edges, &Edges::visit}))
                {
                    for (auto &resident : residents)
                    {
                        resident.retention = EPartitionRetention::UNKNOWN_PAYLOAD;
                    }
                    return;
                }
            }
        }
        bool changed{};
        do
        {
            changed = false;
            for (const auto &[source, target] : edges.values)
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
            ++stats.unloaded;
            context.invalidated = true;
        }
        recount();
    }

    void adopt(SceneStageContext &context)
    {
        if (!context.allow_structure)
        {
            return;
        }
        std::vector<std::shared_ptr<Read>> batch;
        std::size_t objects{}, source_bytes{};
        const bool required_batch = requiredMissing();
        for (const auto &[ordinal, required] : wanted)
        {
            if (find(ordinal) || (required_batch && !required))
            {
                continue;
            }
            const auto found = std::ranges::find_if(reads, [&](const auto &read) {
                return !read->obsolete && read->source_epoch == source_epoch && read->partition.value == ordinal;
            });
            if (found == reads.end() || (*found)->state.load(std::memory_order_acquire) == Read::State::PENDING)
            {
                return; // One complete explicit set; no partition installs prematurely.
            }
            const auto &read = *found;
            const auto state = read->state.load(std::memory_order_acquire);
            if (state != Read::State::VALUE)
            {
                fail(state == Read::State::ERROR
                         ? WorldLoadingFailure{EWorldLoadingError::READ_FAILURE, read->partition, read->error}
                         : WorldLoadingFailure{EWorldLoadingError::CANCELLED, read->partition});
                return;
            }
            objects += read->data->objectCount();
            source_bytes += read->data->retainedBytes();
            batch.push_back(read);
        }
        if (batch.empty())
        {
            return;
        }
        const auto &limits = services.limits;
        if (batch.size() > limits.partitions - stats.resident_partitions ||
            objects > limits.entities - stats.resident_entities ||
            source_bytes > limits.source_bytes - stats.resident_source_bytes)
        {
            fail({EWorldLoadingError::CAPACITY, batch.front()->partition});
            return;
        }
        std::vector<world::WorldPartitionObjectView> input;
        input.reserve(objects);
        for (const auto &read : batch)
        {
            for (std::size_t index{}; index < read->data->objectCount(); ++index)
            {
                input.push_back(read->data->objectAt(index));
            }
        }
        auto source_owner = std::make_shared<loading::WorldStorageSource>(services.source);
        auto world = std::shared_ptr<const world::WorldDescription>(source_owner, &source_owner->world());
        auto materializer = WorldMaterializer::create(std::move(world), schemas);
        if (!materializer)
        {
            fail({EWorldLoadingError::MATERIALIZE_FAILURE, batch.front()->partition, materializer.error()});
            return;
        }
        std::vector<ecs::Entity> created;
        std::size_t bytes{};
        auto loaded = materializer->objects(registry, identities, input, &created,
                                            limits.component_bytes - stats.resident_component_bytes, &bytes);
        if (!loaded)
        {
            std::size_t first{};
            Ordinal ordinal = batch.front()->partition;
            for (const auto &read : batch)
            {
                if (loaded.error().object < first + read->data->objectCount())
                {
                    ordinal = read->partition;
                    break;
                }
                first += read->data->objectCount();
            }
            fail({loaded.error().code == EWorldMaterializeError::CAPACITY ? EWorldLoadingError::CAPACITY
                                                                          : EWorldLoadingError::MATERIALIZE_FAILURE,
                  ordinal, loaded.error()});
            ++stats.rejected;
            return;
        }
        std::size_t first{};
        for (const auto &read : batch)
        {
            Resident resident;
            const auto count = read->data->objectCount();
            resident.entities.assign(created.begin() + first, created.begin() + first + count);
            // Actual installed inline size is available per schema operation.
            for (std::size_t index{}; index < count; ++index)
            {
                const auto object = read->data->objectAt(index);
                for (std::size_t data{}; data < object.dataCount(); ++data)
                {
                    const auto &id = services.source.world().schemas()[object.schemaOrdinalAt(data)];
                    const auto *schema = schemas.find(ecs::componentSchemaId(id.name));
                    resident.unknown |= schema == nullptr;
                    if (schema)
                    {
                        resident.component_bytes += schema->operations.valueBytes();
                    }
                }
            }
            resident.source = std::move(read->data);
            for (const auto entity : resident.entities)
            {
                membership.emplace(entity, Membership{resident.source->partition().value, componentBytes(entity)});
            }
            residents.push_back(std::move(resident));
            read->obsolete = true;
            first += count;
            ++stats.adopted;
        }
        std::erase_if(reads, [](const auto &read) {
            return read->obsolete && read->state.load(std::memory_order_acquire) != Read::State::PENDING;
        });
        result = {};
        context.invalidated = true;
        recount();
    }

    SceneStageResult maintain(SceneStageContext &context)
    {
        if (!services.source || (!result && result.error().code == EWorldLoadingError::INVALID_CONFIGURATION))
        {
            return lux::cxx::unexpected(
                SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, result.error()});
        }
        if (context.stop.stop_requested())
        {
            stopping = true;
            for (const auto &read : reads)
            {
                read->obsolete = true;
                read->cancel.request_stop();
            }
        }
        std::erase_if(reads, [](const auto &read) {
            return read->obsolete && read->state.load(std::memory_order_acquire) != Read::State::PENDING;
        });
        updateAccounting();
        recount();
        if (stopping)
        {
            return ESceneProgress::COMPLETE;
        }
        refreshDemands();
        unload(context);
        if (!result && result.error().code == EWorldLoadingError::CAPACITY)
        {
            result = {}; // Capacity can recover after a protection token is released.
        }
        if (!result)
        {
            return progress();
        }
        std::size_t starts{};
        // Required demand is admitted before optional work, preserving finite starts.
        for (const bool required_pass : {true, false})
        {
            for (const auto &[ordinal, required] : wanted)
            {
                if (required != required_pass)
                {
                    continue;
                }
                if (find(ordinal) || std::ranges::any_of(reads, [ordinal, this](const auto &read) {
                        return read->partition.value == ordinal && read->source_epoch == source_epoch;
                    }))
                {
                    continue;
                }
                const auto &limits = services.limits;
                if (starts == limits.requests_per_turn || stats.in_flight == limits.in_flight)
                {
                    break;
                }
                if (stats.reserved_read_bytes + stats.staged_bytes > limits.staging_bytes ||
                    limits.read_bytes > limits.staging_bytes - stats.reserved_read_bytes - stats.staged_bytes)
                {
                    fail({EWorldLoadingError::CAPACITY, {ordinal}});
                    break;
                }
                auto read = std::make_shared<Read>();
                read->partition = {ordinal};
                read->source_epoch = source_epoch;
                auto value = stdexec::then(loading::loadWorldPartition(services.source, read->partition,
                                                                       limits.read_bytes, read->cancel.get_token()),
                                           [read](world::WorldPartitionData data) noexcept {
                                               read->data =
                                                   std::make_shared<const world::WorldPartitionData>(std::move(data));
                                               read->state.store(Read::State::VALUE, std::memory_order_release);
                                           });
                auto error =
                    stdexec::upon_error(std::move(value), [read](loading::WorldStorageRuntimeFailure failure) noexcept {
                        read->error = failure;
                        read->state.store(Read::State::ERROR, std::memory_order_release);
                    });
                auto task = stdexec::upon_stopped(std::move(error), [read]() noexcept {
                    read->state.store(Read::State::CANCELLED, std::memory_order_release);
                });
                auto accepted = services.tasks.start(std::move(task));
                if (!accepted)
                {
                    fail({EWorldLoadingError::TASK_REJECTED, read->partition, accepted.error()});
                    break;
                }
                reads.push_back(std::move(read));
                ++stats.reads;
                ++starts;
                recount();
            }
        }
        if (stats.staged_bytes > services.limits.staging_bytes)
        {
            fail({EWorldLoadingError::CAPACITY});
        }
        if (result)
        {
            adopt(context);
        }
        return progress();
    }
};

WorldLoadingSystem::WorldLoadingSystem(ecs::Registry &registry, ecs::ComponentSchemaSet schemas,
                                       WorldLoadingServices services)
    : impl_(std::make_unique<Impl>(registry, std::move(schemas), std::move(services)))
{
}
WorldLoadingSystem::~WorldLoadingSystem() noexcept = default;
SceneStageResult WorldLoadingSystem::maintain(SceneStageContext &context) noexcept
{
    return impl_->maintain(context);
}
WorldLoadingResult<void> WorldLoadingSystem::replaceSource(loading::WorldStorageSource source)
{
    auto &d = *impl_;
    if (!source)
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_CONFIGURATION});
    }
    if (!d.residents.empty())
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::SOURCE_BUSY});
    }
    if (d.source_epoch == (std::numeric_limits<std::uint64_t>::max)())
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::CAPACITY});
    }
    for (const auto &read : d.reads)
    {
        read->obsolete = true;
        read->cancel.request_stop();
    }
    d.services.source = std::move(source);
    ++d.source_epoch;
    d.demand_dirty = true;
    d.result = {};
    return {};
}
WorldLoadingResult<PartitionRetention> WorldLoadingSystem::retain(Ordinal partition)
{
    if (auto *resident = impl_->find(partition.value))
    {
        return PartitionRetention(resident->protection);
    }
    return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::NOT_RESIDENT, partition});
}
bool WorldLoadingSystem::partitionOf(ecs::Entity entity, Ordinal &partition) const noexcept
{
    if (const auto found = impl_->membership.find(entity); found != impl_->membership.end())
    {
        partition = {found->second.partition};
        return true;
    }
    return false;
}
WorldLoadingResult<void> WorldLoadingSystem::setDirty(Ordinal partition, bool dirty)
{
    if (auto *resident = impl_->find(partition.value))
    {
        resident->dirty = dirty;
        impl_->has_dirty |= dirty;
        return {};
    }
    return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::NOT_RESIDENT, partition});
}
void WorldLoadingSystem::clearDirty() noexcept
{
    if (std::exchange(impl_->has_dirty, false))
    {
        for (auto &resident : impl_->residents)
        {
            resident.dirty = false;
        }
    }
}
WorldLoadingResult<void> WorldLoadingSystem::retry(Ordinal partition)
{
    auto &d = *impl_;
    if (!d.wanted.contains(partition.value))
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_PARTITION, partition});
    }
    for (const auto &read : d.reads)
    {
        if (read->partition == partition && read->state.load(std::memory_order_acquire) != Impl::Read::State::VALUE)
        {
            read->obsolete = true;
            read->cancel.request_stop();
        }
    }
    d.result = {};
    return {};
}
WorldLoadingResult<void> WorldLoadingSystem::validateAdditional(std::size_t entities, std::size_t bytes) const noexcept
{
    const auto &d = *impl_;
    std::size_t retained{};
    for (const auto &resident : d.residents)
    {
        retained += resident.component_bytes;
    }
    const auto &limits = d.services.limits;
    if (entities > limits.entities - (std::min)(limits.entities, d.membership.size()) ||
        bytes > limits.component_bytes - (std::min)(limits.component_bytes, retained))
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::CAPACITY});
    }
    return {};
}
WorldLoadingResult<void> WorldLoadingSystem::adoptCaptured(
    std::span<const std::shared_ptr<const world::WorldPartitionData>> partitions)
{
    auto &d = *impl_;
    if (!d.result)
    {
        return d.result;
    }
    if (!d.residents.empty() || !d.reads.empty() || d.identities.size() != 0)
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::SOURCE_BUSY});
    }
    d.refreshDemands();
    if (!d.result)
    {
        return d.result;
    }
    for (const auto &partition : partitions)
    {
        if (!partition || partition->partition().value >= d.services.source.world().partitionCount() ||
            partition->bundle() != d.services.source.world().bundleId() ||
            partition->generation() != d.services.source.world().generation() ||
            std::ranges::any_of(d.reads, [&](const auto &read) { return read->partition == partition->partition(); }))
        {
            d.reads.clear();
            return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_PARTITION,
                                                            partition ? partition->partition() : Ordinal{}});
        }
        auto read = std::make_shared<Impl::Read>();
        read->partition = partition->partition();
        read->source_epoch = d.source_epoch;
        read->data = partition;
        read->state.store(Impl::Read::State::VALUE, std::memory_order_release);
        d.reads.push_back(std::move(read));
    }
    for (const auto &[ordinal, required] : d.wanted)
    {
        if (required &&
            std::ranges::none_of(d.reads, [ordinal](const auto &read) { return read->partition.value == ordinal; }))
        {
            d.reads.clear();
            d.recount();
            return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::NOT_RESIDENT, {ordinal}});
        }
    }
    d.recount();
    if (d.stats.staged_bytes > d.services.limits.staging_bytes)
    {
        d.reads.clear();
        d.recount();
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::CAPACITY});
    }
    std::size_t publications{};
    std::size_t resource_steps{};
    SceneStageContext context{publications, resource_steps};
    d.adopt(context);
    return d.result;
}
ecs::WorldEntityMap &WorldLoadingSystem::identities() noexcept
{
    return impl_->identities;
}
WorldLoadingResult<void> WorldLoadingSystem::replaceEntities(Ordinal partition, std::vector<ecs::Entity> entities)
{
    auto &d = *impl_;
    auto *resident = d.find(partition.value);
    if (!resident)
    {
        return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::NOT_RESIDENT, partition});
    }
    std::size_t bytes{};
    std::unordered_map<ecs::Entity, Impl::Membership> prepared;
    prepared.reserve(entities.size());
    for (const auto entity : entities)
    {
        const auto found = d.membership.find(entity);
        if (!d.registry.valid(entity) || prepared.contains(entity) ||
            (found != d.membership.end() && found->second.partition != partition.value))
        {
            return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_CONFIGURATION, partition});
        }
        const auto size = d.componentBytes(entity);
        bytes += size;
        prepared.emplace(entity, Impl::Membership{partition.value, size});
    }
    for (const auto entity : resident->entities)
    {
        d.membership.erase(entity);
    }
    d.membership.merge(prepared);
    resident->entities = std::move(entities);
    resident->component_bytes = bytes;
    resident->dirty = true;
    d.has_dirty = true;
    d.recount();
    return {};
}
const ecs::WorldEntityMap &WorldLoadingSystem::identities() const noexcept
{
    return impl_->identities;
}
const WorldLoadingResult<void> &WorldLoadingSystem::status() const noexcept
{
    return impl_->result;
}
WorldLoadingStatistics WorldLoadingSystem::statistics() const noexcept
{
    return impl_->stats;
}
const world::WorldPartitionData *WorldLoadingSystem::source(Ordinal partition) const noexcept
{
    const auto *resident = impl_->find(partition.value);
    return resident ? resident->source.get() : nullptr;
}
std::vector<ResidentPartition> WorldLoadingSystem::resident() const
{
    std::vector<ResidentPartition> result;
    result.reserve(impl_->residents.size());
    for (const auto &resident : impl_->residents)
    {
        result.push_back({resident.source->partition(), resident.entities.size(), resident.source->retainedBytes(),
                          resident.component_bytes, resident.retention});
    }
    return result;
}
SceneSystemRegistration worldLoadingSystemRegistration() noexcept
{
    static constexpr std::array requirements{SceneSystemRequirementSpec{
        "world_loading", "lux.world.loading", lux::cxx::typeToken<WorldLoadingServices>(), false}};
    return {.type = system::systemTypeId(WorldLoadingSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<WorldLoadingSystem>(),
            .description = &WorldLoadingSystem::Description,
            .configuration = worldLoadingConfigurationCodec(),
            .requirements = requirements,
            .install = +[](SceneBuilder &builder,
                           SceneSystemDescription input) noexcept -> lux::cxx::expected<void, SceneSystemBuildFailure> {
                auto *services = builder.require<WorldLoadingServices>(input.instanceId(), "world_loading");
                if (!services || !services->source)
                {
                    return lux::cxx::unexpected(
                        SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT, input.instanceId()});
                }
                auto configuration = builder.decodeConfiguration<WorldLoadingConfiguration>(input);
                if (!configuration)
                {
                    return lux::cxx::unexpected(configuration.error());
                }
                auto configured = *services;
                configured.bootstrap.insert(configured.bootstrap.end(), configuration->bootstrap.begin(),
                                            configuration->bootstrap.end());
                auto instance = builder.emplaceSystem<WorldLoadingSystem>(
                    input.instanceId(), builder.registry(), builder.components(), std::move(configured));
                if (!instance)
                {
                    return lux::cxx::unexpected(instance.error());
                }
                if (!(**instance).status())
                {
                    return lux::cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION,
                                                                        input.instanceId(),
                                                                        {},
                                                                        0,
                                                                        {},
                                                                        (**instance).status().error()});
                }
                return builder.addMaintenanceTask<WorldLoadingSystem>(
                    input.instanceId(), [](WorldLoadingSystem &system, SceneStageContext &context) noexcept {
                        return system.maintain(context);
                    });
            }};
}
} // namespace lux::scene
