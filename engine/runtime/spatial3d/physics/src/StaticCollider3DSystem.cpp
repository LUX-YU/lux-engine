#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DSystem.hpp>

#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DSystem.hpp>
#include <lux/engine/ecs/physics3d/StaticColliderBatch3DCodec.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::runtime::spatial3d
{
    namespace
    {
        [[nodiscard]] std::uint32_t entityKey(
            entt::entity entity) noexcept
        {
            return static_cast<std::uint32_t>(entt::to_integral(entity));
        }

        [[nodiscard]] bool supportedContent(
            const lux::ecs::scene_format::ContentBlobRef& content) noexcept
        {
            return content.valid() &&
                content.type.name() ==
                    lux::physics3d::kStaticColliderBatch3DContentTypeName &&
                content.schema_version ==
                    lux::physics3d::kStaticColliderBatch3DSchemaVersion;
        }

        [[nodiscard]] bool sameTransform(
            const lux::ecs::ResolvedTransform3DComponent& lhs,
            const lux::ecs::ResolvedTransform3DComponent& rhs) noexcept
        {
            return lhs.position == rhs.position &&
                (lhs.linear.array() == rhs.linear.array()).all();
        }

        [[nodiscard]] EStaticCollider3DFailure domainFailure(
            EStaticCollider3DPrepareError error) noexcept
        {
            switch (error)
            {
            case EStaticCollider3DPrepareError::DECODE_FAILED:
                return EStaticCollider3DFailure::DECODE_FAILED;
            case EStaticCollider3DPrepareError::INVALID_TRANSFORM:
                return EStaticCollider3DFailure::INVALID_TRANSFORM;
            case EStaticCollider3DPrepareError::INVALID_REQUEST:
            case EStaticCollider3DPrepareError::PREPARE_FAILED:
                return EStaticCollider3DFailure::PREPARE_FAILED;
            }
            return EStaticCollider3DFailure::PREPARE_FAILED;
        }

        struct CompletionControl final
        {
            std::atomic<StaticCollider3DSystem*> owner{nullptr};
        };
    } // namespace

    Physics3DSceneSnapshot Physics3DSceneService::snapshot() const noexcept
    {
        if (!scene)
            return {};
        const auto memory = scene->memorySnapshot();
        return {
            scene->dynamicBodyCount(),
            scene->characterCount(),
            scene->staticHeightfieldBodyCount(),
            memory.capacity_bytes,
            memory.allocation_count,
            scene->droppedContactFactCount()};
    }

    struct StaticCollider3DSystem::Impl final
    {
        struct Entry final
        {
            entt::entity entity{entt::null};
            lux::ecs::scene_format::ContentBlobRef content;
            lux::ecs::ResolvedTransform3DComponent transform;
            std::uint64_t generation{0u};
            EStaticCollider3DState state{
                EStaticCollider3DState::WAITING_CONTENT};
            EStaticCollider3DFailure failure{
                EStaticCollider3DFailure::NONE};
            lux::runtime::entity_scene::ContentBlobLease candidate_content;
            StaticCollider3DPrepareBudgetLease candidate_budget;
            std::unique_ptr<lux::ecs::Physics3DPreparedStaticBatch> prepared;
            std::unique_ptr<lux::ecs::Physics3DStaticBatchStager> stager;
            std::shared_ptr<StaticCollider3DBinding> candidate;
            std::shared_ptr<StaticCollider3DBinding> active;
            std::shared_ptr<StaticCollider3DBinding> superseded;
            bool status_dirty{false};
            bool publish_queued{false};
            bool arm_queued{false};
            bool transients_ready{false};
            bool discard_candidate{false};
        };

        Impl(
            StaticCollider3DSystem& owner_value,
            lux::exec::AsyncRuntime& async_runtime_value,
            lux::exec::AsyncScope& scene_scope_value,
            StaticCollider3DPrepareClient preparation_value,
            std::shared_ptr<lux::ecs::Physics3DScene> scene_value,
            lux::runtime::entity_scene::ContentBlobClient content_value,
            StaticCollider3DSystemConfig config_value)
            : async_runtime(&async_runtime_value),
              scope(&scene_scope_value),
              preparation(std::move(preparation_value)),
              scene(std::move(scene_value)),
              content(std::move(content_value)),
              completion(std::make_shared<CompletionControl>()),
              config(config_value)
        {
            if (!preparation || !scene || !content ||
                config.maximum_staged_bodies_per_update == 0u ||
                config.maximum_tracked_batches == 0u ||
                config.maximum_retirement_units_per_update == 0u ||
                config.maximum_tracked_batches >
                    (std::numeric_limits<std::size_t>::max)() / 4u)
            {
                std::abort();
            }
            completion->owner.store(&owner_value, std::memory_order_release);
            entries.reserve(config.maximum_tracked_batches);
            const auto event_capacity = static_cast<std::size_t>(
                config.maximum_tracked_batches) * 4u;
            maximum_signal_events = event_capacity;
            maximum_owner_slots = static_cast<std::size_t>(
                config.maximum_tracked_batches) * 2u;
            maximum_retirement_bindings = maximum_owner_slots;
            dirty.reserve(event_capacity);
            dirty_scratch.reserve(event_capacity);
            retirement.reserve(maximum_retirement_bindings);
        }

        [[nodiscard]] std::uint64_t nextGeneration() noexcept
        {
            ++next_generation;
            if (next_generation == 0u)
                ++next_generation;
            return next_generation;
        }

        void transition(
            Entry& entry,
            EStaticCollider3DState state,
            EStaticCollider3DFailure failure =
                EStaticCollider3DFailure::NONE) noexcept
        {
            entry.state = state;
            entry.failure = failure;
            entry.status_dirty = true;
        }

        void retireStager(Entry& entry) noexcept
        {
            if (!entry.stager)
                return;
            auto physics = entry.stager->cancel();
            entry.stager.reset();
            if (entry.prepared)
                std::abort();
            if (!physics || physics->remainingRetirementUnits() == 0u)
                return;
            if (!entry.candidate_content || !entry.candidate_budget)
                std::abort();
            auto partial = StaticCollider3DSystem::makeBinding(
                entry.generation,
                scene,
                std::move(entry.candidate_content),
                std::move(entry.candidate_budget),
                std::move(physics));
            retire(partial);
        }

        void retirePrepared(Entry& entry) noexcept
        {
            if (!entry.prepared)
                return;
            if (!entry.candidate_content || !entry.candidate_budget)
                std::abort();
            auto physics = scene->makeStaticHeightfieldRetirement(
                std::move(entry.prepared), entry.entity);
            if (!physics || physics->remainingRetirementUnits() == 0u)
                std::abort();
            auto unadopted = StaticCollider3DSystem::makeBinding(
                entry.generation,
                scene,
                std::move(entry.candidate_content),
                std::move(entry.candidate_budget),
                std::move(physics));
            retire(unadopted);
        }

        void retireUntrackedPrepared(
            entt::entity entity,
            std::uint64_t generation,
            PreparedStaticCollider3D prepared) noexcept
        {
            if (!prepared.batch || !prepared.budget || generation == 0u)
                std::abort();
            auto physics = scene->makeStaticHeightfieldRetirement(
                std::move(prepared.batch), entity);
            if (!physics || physics->remainingRetirementUnits() == 0u)
                std::abort();
            auto untracked = StaticCollider3DSystem::makeBinding(
                generation,
                scene,
                {},
                std::move(prepared.budget),
                std::move(physics));
            retire(untracked);
        }

        void fail(
            Entry& entry,
            EStaticCollider3DFailure failure) noexcept
        {
            cancelCandidate(entry);
            transition(entry, EStaticCollider3DState::FAILED, failure);
            ++metrics.failed_preparations;
        }

        [[nodiscard]] bool mark(entt::entity entity) noexcept
        {
            if (std::ranges::find(dirty, entity) != dirty.end())
            {
                ++metrics.coalesced_changes;
                return true;
            }
            if (dirty.size() >= maximum_signal_events)
            {
                ++metrics.observer_overflows;
                rescan_required = true;
                return false;
            }
            dirty.push_back(entity);
            return true;
        }

        void retire(
            const std::shared_ptr<StaticCollider3DBinding>& binding) noexcept
        {
            if (!binding)
                return;
            const bool was_active = binding->active();
            if (!binding->beginRetirement())
                return;
            if (was_active)
                ++metrics.immediate_hides;
            if (retirement.size() >= maximum_retirement_bindings)
                std::abort();
            retirement.push_back(binding);
            ++metrics.retirement_enqueues;
        }

        void cancelCandidate(Entry& entry) noexcept
        {
            retireStager(entry);
            retirePrepared(entry);
            entry.candidate_budget = {};
            if (entry.candidate)
                retire(entry.candidate);
            entry.candidate.reset();
            entry.candidate_content = {};
            entry.publish_queued = false;
            entry.arm_queued = false;
            entry.discard_candidate = false;
        }

        void retirePublished(Entry& entry) noexcept
        {
            if (entry.candidate)
                retire(entry.candidate);
            entry.candidate.reset();
            retire(entry.active);
            entry.active.reset();
            retire(entry.superseded);
            entry.superseded.reset();
            entry.publish_queued = false;
            entry.discard_candidate = false;
        }

        void retireEntry(Entry& entry) noexcept
        {
            cancelCandidate(entry);
            retirePublished(entry);
        }

        void drainRetirement() noexcept
        {
            std::uint32_t budget =
                config.maximum_retirement_units_per_update;
            std::size_t write = 0u;
            for (auto& binding : retirement)
            {
                if (!binding || binding->retired())
                    continue;
                if (budget != 0u)
                {
                    const auto before =
                        binding->remainingRetirementUnits();
                    const bool complete = binding->retireSome(budget);
                    const auto after =
                        binding->remainingRetirementUnits();
                    const auto retired = before >= after ? before - after : 0u;
                    budget = retired >= budget ? 0u : budget - retired;
                    if (complete)
                    {
                        ++metrics.retired_batches;
                        rescan_required = true;
                        continue;
                    }
                }
                retirement[write++] = std::move(binding);
            }
            retirement.resize(write);
        }

        [[nodiscard]] std::size_t ownerSlotCount() const noexcept
        {
            std::size_t result = retirement.size();
            for (const auto& [_, entry] : entries)
            {
                result += entry.candidate ? 1u : 0u;
                result += entry.active ? 1u : 0u;
                result += entry.superseded ? 1u : 0u;
                if (entry.candidate_content || entry.candidate_budget ||
                    entry.prepared || entry.stager)
                {
                    ++result;
                }
            }
            return result;
        }

        void discardStaleCandidates() noexcept
        {
            for (auto& [_, entry] : entries)
            {
                if (!entry.discard_candidate)
                    continue;
                cancelCandidate(entry);
                transition(
                    entry,
                    EStaticCollider3DState::FAILED,
                    EStaticCollider3DFailure::PREPARE_FAILED);
                (void)mark(entry.entity);
            }
        }

        void enqueueSuperseded() noexcept
        {
            for (auto& [_, entry] : entries)
            {
                if (!entry.superseded)
                    continue;
                retire(entry.superseded);
                entry.superseded.reset();
            }
        }

        void onFactConstructed(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            (void)mark(entity);
        }

        void onFactUpdated(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            hideUncommittedCandidate(entity);
            (void)mark(entity);
        }

        void onFactDestroyed(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            if (const auto* binding = registry.try_get<
                    StaticCollider3DBindingComponent>(entity))
            {
                if (binding->binding)
                    hideBinding(binding->binding);
            }
            hideTracked(entity);
            (void)mark(entity);
        }

        void onTransformChanged(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            if (registry.all_of<lux::ecs::StaticColliderBatch3DComponent>(
                    entity))
            {
                hideUncommittedCandidate(entity);
                (void)mark(entity);
            }
        }

        void onTransformDestroyed(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            if (const auto* binding = registry.try_get<
                    StaticCollider3DBindingComponent>(entity))
            {
                if (binding->binding)
                    hideBinding(binding->binding);
            }
            hideTracked(entity);
            if (registry.all_of<lux::ecs::StaticColliderBatch3DComponent>(
                    entity))
            {
                (void)mark(entity);
            }
        }

        void onBindingDestroyed(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            // EnTT dispatches on_destroy while the binding is still readable.
            // Hide synchronously; the coalesced owner update decides whether
            // this was entity/fact removal (retire) or an externally removed
            // transient that must be re-armed.
            if (const auto* binding = registry.try_get<
                    StaticCollider3DBindingComponent>(entity))
            {
                if (binding->binding)
                    hideBinding(binding->binding);
            }
            hideTracked(entity);
            (void)mark(entity);
        }

        void onStatusDestroyed(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            if (registry.all_of<lux::ecs::StaticColliderBatch3DComponent>(
                    entity))
            {
                hideUncommittedCandidate(entity);
                (void)mark(entity);
            }
        }

        void hideBinding(
            const std::shared_ptr<StaticCollider3DBinding>& binding) noexcept
        {
            if (!binding)
                return;
            if (binding->active())
                ++metrics.immediate_hides;
            binding->hide();
        }

        void hideTracked(entt::entity entity) noexcept
        {
            const auto found = entries.find(entityKey(entity));
            if (found == entries.end() || found->second.entity != entity)
                return;
            auto& entry = found->second;
            if (entry.candidate)
                hideBinding(entry.candidate);
            if (entry.active)
                hideBinding(entry.active);
            if (entry.superseded)
                hideBinding(entry.superseded);
        }

        void hideUncommittedCandidate(entt::entity entity) noexcept
        {
            const auto found = entries.find(entityKey(entity));
            if (found == entries.end() || found->second.entity != entity)
                return;
            if (found->second.candidate)
                hideBinding(found->second.candidate);
        }

        void attach(
            lux::ecs::Registry& registry,
            lux::ecs::EcsCommandWriter writer)
        {
            if (attached)
                std::abort();
            attached = &registry;
            commands = writer;
            registry.storage<StaticCollider3DBindingComponent>().reserve(
                config.maximum_tracked_batches);
            registry.storage<StaticCollider3DStatusComponent>().reserve(
                config.maximum_tracked_batches);
            fact_constructed = registry
                .on_construct<lux::ecs::StaticColliderBatch3DComponent>()
                .connect<&Impl::onFactConstructed>(*this);
            fact_updated = registry
                .on_update<lux::ecs::StaticColliderBatch3DComponent>()
                .connect<&Impl::onFactUpdated>(*this);
            fact_destroyed = registry
                .on_destroy<lux::ecs::StaticColliderBatch3DComponent>()
                .connect<&Impl::onFactDestroyed>(*this);
            transform_constructed = registry
                .on_construct<lux::ecs::ResolvedTransform3DComponent>()
                .connect<&Impl::onTransformChanged>(*this);
            transform_updated = registry
                .on_update<lux::ecs::ResolvedTransform3DComponent>()
                .connect<&Impl::onTransformChanged>(*this);
            transform_destroyed = registry
                .on_destroy<lux::ecs::ResolvedTransform3DComponent>()
                .connect<&Impl::onTransformDestroyed>(*this);
            binding_destroyed = registry
                .on_destroy<StaticCollider3DBindingComponent>()
                .connect<&Impl::onBindingDestroyed>(*this);
            status_destroyed = registry
                .on_destroy<StaticCollider3DStatusComponent>()
                .connect<&Impl::onStatusDestroyed>(*this);

            // Signals do not replay, so fold existing facts through the same
            // dirty path used by later construction/update notifications.
            for (const auto entity : registry.view<
                     const lux::ecs::StaticColliderBatch3DComponent>())
            {
                (void)mark(entity);
            }
        }

        void detach(
            lux::ecs::Registry&) noexcept
        {
            fact_constructed.release();
            fact_updated.release();
            fact_destroyed.release();
            transform_constructed.release();
            transform_updated.release();
            transform_destroyed.release();
            binding_destroyed.release();
            status_destroyed.release();
            for (auto& [_, entry] : entries)
                retireEntry(entry);
            entries.clear();
            dirty.clear();
            dirty_scratch.clear();
            attached = nullptr;
            commands = {};
        }

        void beginDesired(
            lux::ecs::Registry& registry,
            entt::entity entity,
            const lux::ecs::StaticColliderBatch3DComponent& fact,
            const lux::ecs::ResolvedTransform3DComponent& transform)
            noexcept
        {
            const auto key = entityKey(entity);
            auto existing_entry = entries.find(key);
            if (existing_entry == entries.end() &&
                entries.size() >= config.maximum_tracked_batches)
            {
                ++metrics.capacity_rejections;
                if (const auto* binding = registry.try_get<
                        StaticCollider3DBindingComponent>(entity);
                    binding && binding->binding)
                {
                    hideBinding(binding->binding);
                }
                if (!commands.push(StaticCollider3DIntentCommand{
                        EStaticCollider3DIntentAction::REMOVE,
                        entity,
                        0u,
                        EStaticCollider3DState::FAILED,
                        EStaticCollider3DFailure::PREPARE_FAILED}))
                {
                    ++metrics.command_backpressure;
                    (void)mark(entity);
                }
                return;
            }
            auto [found, inserted] = entries.try_emplace(key);
            auto& entry = found->second;
            if (!inserted && entry.entity != entity)
            {
                retireEntry(entry);
                entry = Entry{};
                inserted = true;
            }
            if (!inserted &&
                entry.state != EStaticCollider3DState::FAILED &&
                entry.content == fact.content &&
                sameTransform(entry.transform, transform))
            {
                const auto* binding = registry.try_get<
                    StaticCollider3DBindingComponent>(entity);
                entry.transients_ready = binding &&
                    registry.all_of<StaticCollider3DStatusComponent>(entity) &&
                    binding->binding == entry.active;
                if (!entry.transients_ready)
                {
                    entry.arm_queued = false;
                    entry.status_dirty = true;
                }
                return;
            }
            if (!inserted &&
                entry.state == EStaticCollider3DState::WAITING_BACKGROUND)
            {
                // There is no cancellation API for an accepted typed
                // operation. Keep its generation/content owner until the
                // completion settles, then retire any prepared shapes under
                // the ordinary fixed granule before admitting this revision.
                return;
            }

            cancelCandidate(entry);
            entry.entity = entity;
            entry.content = fact.content;
            entry.transform = transform;
            entry.generation = nextGeneration();
            if (!entry.active)
            {
                if (const auto* existing = registry.try_get<
                        StaticCollider3DBindingComponent>(entity))
                {
                    entry.active = existing->binding;
                }
            }
            const auto* binding = registry.try_get<
                StaticCollider3DBindingComponent>(entity);
            entry.transients_ready = binding &&
                registry.all_of<StaticCollider3DStatusComponent>(entity) &&
                binding->binding == entry.active;
            entry.arm_queued = false;
            transition(entry, EStaticCollider3DState::WAITING_CONTENT);

            if (!supportedContent(fact.content))
            {
                fail(entry, EStaticCollider3DFailure::INVALID_REFERENCE);
                return;
            }
            if (ownerSlotCount() >= maximum_owner_slots)
            {
                ++metrics.capacity_rejections;
                transition(
                    entry,
                    EStaticCollider3DState::FAILED,
                    EStaticCollider3DFailure::PREPARE_FAILED);
                // Bounded retirement will set this flag again when a slot is
                // actually freed; retaining it here also handles a slot freed
                // later in this same owner tick.
                rescan_required = true;
                return;
            }
            auto lease = content.resolve(fact.content);
            if (!lease)
            {
                fail(entry, EStaticCollider3DFailure::CONTENT_UNAVAILABLE);
                return;
            }
            entry.candidate_content = std::move(*lease);
        }

        void queueRemove(
            entt::entity entity,
            std::uint64_t generation) noexcept
        {
            if (!registryEntityValid(entity))
                return;
            if (!commands.push(StaticCollider3DIntentCommand{
                    EStaticCollider3DIntentAction::REMOVE,
                    entity,
                    generation,
                    EStaticCollider3DState::WAITING_CONTENT,
                    EStaticCollider3DFailure::NONE}))
            {
                ++metrics.command_backpressure;
                (void)mark(entity);
            }
        }

        [[nodiscard]] bool registryEntityValid(
            entt::entity entity) const noexcept
        {
            return attached && attached->valid(entity);
        }

        [[nodiscard]] bool desiredMatches(const Entry& entry) const noexcept
        {
            if (!attached || !attached->valid(entry.entity))
                return false;
            const auto* fact = attached->try_get<
                lux::ecs::StaticColliderBatch3DComponent>(entry.entity);
            const auto* transform = attached->try_get<
                lux::ecs::ResolvedTransform3DComponent>(entry.entity);
            return fact && transform && fact->content == entry.content &&
                sameTransform(*transform, entry.transform);
        }

        void reconcile(
            lux::ecs::Registry& registry,
            entt::entity entity) noexcept
        {
            const auto key = entityKey(entity);
            auto found = entries.find(key);
            const auto* fact = registry.valid(entity)
                ? registry.try_get<
                      lux::ecs::StaticColliderBatch3DComponent>(entity)
                : nullptr;
            const auto* transform = registry.valid(entity)
                ? registry.try_get<
                      lux::ecs::ResolvedTransform3DComponent>(entity)
                : nullptr;
            if (!fact || !transform)
            {
                std::uint64_t generation = 0u;
                if (found != entries.end() && found->second.entity == entity)
                {
                    generation = found->second.generation;
                    if (found->second.state ==
                        EStaticCollider3DState::WAITING_BACKGROUND)
                    {
                        retirePublished(found->second);
                        queueRemove(entity, generation);
                        return;
                    }
                    retireEntry(found->second);
                    entries.erase(found);
                    // A freed hard-cap slot may admit a previously rejected
                    // desired fact which has no further signal edge.
                    rescan_required = true;
                }
                queueRemove(entity, generation);
                return;
            }
            beginDesired(registry, entity, *fact, *transform);
        }

        void processDirty(lux::ecs::Registry& registry) noexcept
        {
            if (rescan_required)
            {
                rescan_required = false;
                for (auto iterator = entries.begin();
                     iterator != entries.end();)
                {
                    const auto entity = iterator->second.entity;
                    const auto* fact = registry.valid(entity)
                        ? registry.try_get<
                              lux::ecs::StaticColliderBatch3DComponent>(entity)
                        : nullptr;
                    const auto* transform = registry.valid(entity)
                        ? registry.try_get<
                              lux::ecs::ResolvedTransform3DComponent>(entity)
                        : nullptr;
                    if (!fact || !transform)
                    {
                        const auto generation = iterator->second.generation;
                        if (iterator->second.state ==
                            EStaticCollider3DState::WAITING_BACKGROUND)
                        {
                            retirePublished(iterator->second);
                            queueRemove(entity, generation);
                            ++iterator;
                            continue;
                        }
                        retireEntry(iterator->second);
                        iterator = entries.erase(iterator);
                        queueRemove(entity, generation);
                        continue;
                    }
                    beginDesired(registry, entity, *fact, *transform);
                    ++iterator;
                }
                for (const auto entity : registry.view<
                         const lux::ecs::StaticColliderBatch3DComponent,
                         const lux::ecs::ResolvedTransform3DComponent>())
                {
                    const auto found = entries.find(entityKey(entity));
                    if (found == entries.end() ||
                        found->second.entity != entity)
                    {
                        beginDesired(
                            registry,
                            entity,
                            registry.get<lux::ecs::
                                StaticColliderBatch3DComponent>(entity),
                            registry.get<lux::ecs::
                                ResolvedTransform3DComponent>(entity));
                    }
                }
                dirty.clear();
                return;
            }

            dirty_scratch.clear();
            dirty_scratch.insert(
                dirty_scratch.end(), dirty.begin(), dirty.end());
            dirty.clear();
            for (const auto entity : dirty_scratch)
                reconcile(registry, entity);
            dirty_scratch.clear();
        }

        void submitWaiting() noexcept
        {
            if (closing)
                return;
            for (auto& [_, entry] : entries)
            {
                if (entry.state != EStaticCollider3DState::WAITING_CONTENT ||
                    !entry.candidate_content)
                {
                    continue;
                }
                auto submitted = preparation.execute(BuildStaticCollider3D{
                    entry.candidate_content.bytes(),
                    entry.transform,
                    entry.generation});
                if (!submitted)
                {
                    if (submitted.error() ==
                            lux::async::ESubmitError::QUEUE_FULL ||
                        submitted.error() == lux::async::ESubmitError::
                                                 BYTE_BUDGET_EXHAUSTED)
                    {
                        ++metrics.queue_backpressure;
                        continue;
                    }
                    fail(entry, EStaticCollider3DFailure::PREPARE_FAILED);
                    continue;
                }

                const auto entity = entry.entity;
                const auto generation = entry.generation;
                auto pipeline =
                    std::move(*submitted) |
                    stdexec::continues_on(
                        lux::exec::mainThreadScheduler(*async_runtime)) |
                    stdexec::then(
                        [weak = std::weak_ptr{completion},
                         entity,
                         generation](
                            lux::async::OperationOutcome<BuildStaticCollider3D>
                                outcome) mutable noexcept
                        {
                            const auto locked = weak.lock();
                            if (!locked)
                                return;
                            auto* target = locked->owner.load(
                                std::memory_order_acquire);
                            if (target)
                            {
                                target->acceptPreparation(
                                    entity,
                                    generation,
                                    std::move(outcome));
                            }
                        }) |
                    stdexec::upon_stopped(
                        [weak = std::weak_ptr{completion},
                         entity,
                         generation]() noexcept
                        {
                            const auto locked = weak.lock();
                            if (!locked)
                                return;
                            auto* target = locked->owner.load(
                                std::memory_order_acquire);
                            if (target)
                            {
                                target->acceptPreparationStopped(
                                    entity, generation);
                            }
                        });
                transition(entry, EStaticCollider3DState::WAITING_BACKGROUND);
                if (!lux::exec::spawn(*scope, std::move(pipeline)))
                {
                    transition(entry, EStaticCollider3DState::WAITING_CONTENT);
                    ++metrics.queue_backpressure;
                    continue;
                }
                ++metrics.preparation_attempts;
            }
        }

        void beginStagers() noexcept
        {
            for (auto& [_, entry] : entries)
            {
                if (entry.state != EStaticCollider3DState::STAGING ||
                    entry.stager || !entry.prepared)
                {
                    continue;
                }
                auto stager = scene->beginStaticHeightfieldStaging(
                    std::move(entry.prepared), entry.entity);
                if (!stager)
                {
                    fail(entry, EStaticCollider3DFailure::PREPARE_FAILED);
                    continue;
                }
                entry.stager = std::move(*stager);
            }
        }

        void advanceStagers() noexcept
        {
            std::uint32_t budget =
                config.maximum_staged_bodies_per_update;
            bool progressed = true;
            while (budget != 0u && progressed)
            {
                progressed = false;
                for (auto& [_, entry] : entries)
                {
                    if (budget == 0u)
                        break;
                    if (entry.state != EStaticCollider3DState::STAGING ||
                        !entry.stager)
                    {
                        continue;
                    }
                    auto complete = entry.stager->advance(1u);
                    --budget;
                    progressed = true;
                    if (!complete)
                    {
                        fail(entry, EStaticCollider3DFailure::PREPARE_FAILED);
                        continue;
                    }
                    if (!*complete)
                        continue;
                    auto physics = entry.stager->finish();
                    entry.stager.reset();
                    if (!physics)
                    {
                        fail(entry, EStaticCollider3DFailure::PREPARE_FAILED);
                        continue;
                    }
                    entry.candidate = StaticCollider3DSystem::makeBinding(
                        entry.generation,
                        scene,
                        std::move(entry.candidate_content),
                        std::move(entry.candidate_budget),
                        std::move(physics));
                    transition(entry, EStaticCollider3DState::READY);
                }
            }
        }

        void emitCommands() noexcept
        {
            for (auto& [_, entry] : entries)
            {
                if (!entry.transients_ready)
                {
                    if (!entry.arm_queued &&
                        commands.push(StaticCollider3DIntentCommand{
                            EStaticCollider3DIntentAction::ARM,
                            entry.entity,
                            entry.generation,
                            entry.state,
                            entry.failure}))
                    {
                        entry.arm_queued = true;
                    }
                    else if (!entry.arm_queued)
                    {
                        ++metrics.command_backpressure;
                    }
                    continue;
                }
                if (entry.state == EStaticCollider3DState::READY &&
                    entry.candidate && !entry.publish_queued)
                {
                    if (commands.push(StaticCollider3DIntentCommand{
                            EStaticCollider3DIntentAction::PUBLISH,
                            entry.entity,
                            entry.generation,
                            entry.state,
                            EStaticCollider3DFailure::NONE}))
                    {
                        entry.publish_queued = true;
                        entry.status_dirty = false;
                        // The command slot is already reserved. Establish
                        // backend visibility in the ordinary domain update;
                        // the later barrier only patches pre-armed transient
                        // ownership.
                        StaticCollider3DSystem::publishBinding(
                            entry.candidate);
                    }
                    else
                        ++metrics.command_backpressure;
                    continue;
                }
                if (!entry.status_dirty)
                    continue;
                if (commands.push(StaticCollider3DIntentCommand{
                        EStaticCollider3DIntentAction::STATUS,
                        entry.entity,
                        entry.generation,
                        entry.state,
                        entry.failure}))
                {
                    entry.status_dirty = false;
                }
            }
        }

        void emitCloseCommands(lux::ecs::Registry& registry) noexcept
        {
            // Close uses the same typed command barrier as ordinary removal.
            // Rebuild this bounded snapshot every tick so command-buffer
            // backpressure merely defers the remainder.
            dirty.clear();
            rescan_required = false;
            dirty_scratch.clear();
            const auto append = [this](entt::entity entity) noexcept
            {
                if (std::ranges::find(dirty_scratch, entity) !=
                    dirty_scratch.end())
                {
                    return;
                }
                if (dirty_scratch.size() >= maximum_signal_events)
                    std::abort();
                dirty_scratch.push_back(entity);
            };
            for (const auto entity : registry.view<
                     const StaticCollider3DBindingComponent>())
            {
                append(entity);
            }
            for (const auto entity : registry.view<
                     const StaticCollider3DStatusComponent>())
            {
                append(entity);
            }
            for (const auto entity : dirty_scratch)
            {
                if (!commands.push(StaticCollider3DIntentCommand{
                        EStaticCollider3DIntentAction::REMOVE,
                        entity,
                        0u,
                        EStaticCollider3DState::FAILED,
                        EStaticCollider3DFailure::NONE}))
                {
                    ++metrics.command_backpressure;
                    break;
                }
            }
            dirty_scratch.clear();
        }

        [[nodiscard]] bool hasTransientComponents() const noexcept
        {
            return attached &&
                (!attached->storage<StaticCollider3DBindingComponent>().empty() ||
                 !attached->storage<StaticCollider3DStatusComponent>().empty());
        }

        lux::exec::AsyncRuntime* async_runtime{nullptr};
        lux::exec::AsyncScope* scope{nullptr};
        StaticCollider3DPrepareClient preparation;
        std::shared_ptr<lux::ecs::Physics3DScene> scene;
        lux::runtime::entity_scene::ContentBlobClient content;
        std::shared_ptr<CompletionControl> completion;
        StaticCollider3DSystemConfig config;
        lux::ecs::Registry* attached{nullptr};
        lux::ecs::EcsCommandWriter commands;
        entt::scoped_connection fact_constructed;
        entt::scoped_connection fact_updated;
        entt::scoped_connection fact_destroyed;
        entt::scoped_connection transform_constructed;
        entt::scoped_connection transform_updated;
        entt::scoped_connection transform_destroyed;
        entt::scoped_connection binding_destroyed;
        entt::scoped_connection status_destroyed;
        std::unordered_map<std::uint32_t, Entry> entries;
        std::vector<entt::entity> dirty;
        std::vector<entt::entity> dirty_scratch;
        std::vector<std::shared_ptr<StaticCollider3DBinding>> retirement;
        std::size_t maximum_signal_events{0u};
        std::size_t maximum_owner_slots{0u};
        std::size_t maximum_retirement_bindings{0u};
        std::uint64_t next_generation{0u};
        StaticCollider3DSystemSnapshot metrics;
        bool rescan_required{false};
        bool closing{false};
    };

    StaticCollider3DSystem::StaticCollider3DSystem(
        lux::exec::AsyncRuntime& async_runtime,
        lux::exec::AsyncScope& scene_scope,
        StaticCollider3DPrepareClient preparation,
        std::shared_ptr<lux::ecs::Physics3DScene> scene,
        lux::runtime::entity_scene::ContentBlobClient content,
        StaticCollider3DSystemConfig config)
        : impl_(std::make_unique<Impl>(
              *this,
              async_runtime,
              scene_scope,
              std::move(preparation),
              std::move(scene),
              std::move(content),
              config))
    {}

    StaticCollider3DSystem::~StaticCollider3DSystem()
    {
        impl_->completion->owner.store(nullptr, std::memory_order_release);
        if (impl_->attached)
            impl_->detach(*impl_->attached);
    }

    std::shared_ptr<StaticCollider3DBinding>
    StaticCollider3DSystem::makeBinding(
        std::uint64_t generation,
        std::shared_ptr<lux::ecs::Physics3DScene> scene,
        lux::runtime::entity_scene::ContentBlobLease content,
        StaticCollider3DPrepareBudgetLease budget,
        std::unique_ptr<lux::ecs::Physics3DStaticBatchLease> physics) noexcept
    {
        return std::shared_ptr<StaticCollider3DBinding>{
            new StaticCollider3DBinding{
                generation,
                std::move(scene),
                std::move(content),
                std::move(budget),
                std::move(physics)}};
    }

    void StaticCollider3DSystem::publishBinding(
        const std::shared_ptr<StaticCollider3DBinding>& binding) noexcept
    {
        if (!binding)
            std::abort();
        binding->publish();
    }

    void StaticCollider3DSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void StaticCollider3DSystem::onRemoved(
        const lux::ecs::SystemRemovalContext& removal)
    {
        if (!impl_->closing || !closeComplete())
            std::abort();
        if (impl_->attached == &removal.registry())
            impl_->detach(removal.registry());
    }

    void StaticCollider3DSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        if (impl_->attached != &context.registry())
            std::abort();
        impl_->enqueueSuperseded();
        impl_->discardStaleCandidates();
        impl_->drainRetirement();
        if (impl_->closing)
        {
            impl_->emitCloseCommands(context.registry());
            return;
        }
        impl_->processDirty(context.registry());
        impl_->beginStagers();
        impl_->advanceStagers();
        impl_->submitWaiting();
        impl_->emitCommands();
    }

    std::span<const lux::ecs::ISystem::Type>
    StaticCollider3DSystem::prerequisites() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<lux::ecs::Physics3DSystem>()};
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    StaticCollider3DSystem::runsBefore() const noexcept
    {
        return prerequisites();
    }

    void StaticCollider3DSystem::applyArm(
        lux::ecs::Registry& registry,
        entt::entity entity,
        std::uint64_t generation) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        const auto* fact = registry.valid(entity)
            ? registry.try_get<lux::ecs::StaticColliderBatch3DComponent>(
                  entity)
            : nullptr;
        const auto* transform = registry.valid(entity)
            ? registry.try_get<lux::ecs::ResolvedTransform3DComponent>(entity)
            : nullptr;
        if (found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation || !fact || !transform ||
            fact->content != found->second.content ||
            !sameTransform(*transform, found->second.transform))
        {
            ++impl_->metrics.stale_publications;
            if (found != impl_->entries.end() &&
                found->second.entity == entity &&
                found->second.generation == generation)
            {
                found->second.arm_queued = false;
            }
            if (registry.valid(entity))
                (void)impl_->mark(entity);
            return;
        }

        auto& entry = found->second;
        if (!registry.all_of<StaticCollider3DBindingComponent>(entity))
        {
            registry.emplace<StaticCollider3DBindingComponent>(
                entity,
                StaticCollider3DBindingComponent{entry.active});
        }
        else
        {
            const auto& current = registry.get<
                StaticCollider3DBindingComponent>(entity).binding;
            if (current && current != entry.active)
                std::abort();
            registry.patch<StaticCollider3DBindingComponent>(
                entity,
                [&entry](StaticCollider3DBindingComponent& binding) noexcept
                {
                    binding.binding = entry.active;
                });
        }
        if (!registry.all_of<StaticCollider3DStatusComponent>(entity))
        {
            registry.emplace<StaticCollider3DStatusComponent>(
                entity,
                StaticCollider3DStatusComponent{
                    entry.state, entry.failure, entry.generation});
        }
        else
        {
            registry.patch<StaticCollider3DStatusComponent>(
                entity,
                [&entry](StaticCollider3DStatusComponent& status) noexcept
                {
                    status = {
                        entry.state, entry.failure, entry.generation};
                });
        }
        entry.transients_ready = true;
        entry.arm_queued = false;
        entry.status_dirty = false;
    }

    void StaticCollider3DSystem::applyPublish(
        lux::ecs::Registry& registry,
        entt::entity entity,
        std::uint64_t generation) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        const auto* fact = registry.valid(entity)
            ? registry.try_get<lux::ecs::StaticColliderBatch3DComponent>(
                  entity)
            : nullptr;
        const auto* transform = registry.valid(entity)
            ? registry.try_get<lux::ecs::ResolvedTransform3DComponent>(entity)
            : nullptr;
        if (found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state != EStaticCollider3DState::READY ||
            !found->second.candidate || !fact || !transform ||
            fact->content != found->second.content ||
            !sameTransform(*transform, found->second.transform))
        {
            ++impl_->metrics.stale_publications;
            if (found != impl_->entries.end() &&
                found->second.entity == entity &&
                found->second.generation == generation)
            {
                found->second.publish_queued = false;
                found->second.discard_candidate = true;
            }
            if (registry.valid(entity))
                (void)impl_->mark(entity);
            return;
        }

        auto& entry = found->second;
        auto previous = entry.active;
        if (const auto* bound = registry.try_get<
                StaticCollider3DBindingComponent>(entity))
        {
            if (!previous)
                previous = bound->binding;
            else if (bound->binding != previous)
            {
                ++impl_->metrics.stale_publications;
                entry.publish_queued = false;
                entry.discard_candidate = true;
                (void)impl_->mark(entity);
                return;
            }
        }

        if (!entry.transients_ready || !entry.candidate->active() ||
            !registry.all_of<
                StaticCollider3DBindingComponent,
                StaticCollider3DStatusComponent>(entity))
        {
            ++impl_->metrics.stale_publications;
            entry.publish_queued = false;
            entry.discard_candidate = true;
            (void)impl_->mark(entity);
            return;
        }
        if (entry.superseded)
        {
            ++impl_->metrics.stale_publications;
            entry.publish_queued = false;
            entry.discard_candidate = true;
            (void)impl_->mark(entity);
            return;
        }

        // Backend publication already happened in update(). The barrier only
        // patches armed transient pools. The old visible revision is retained
        // until the next bounded domain update, so a patch never creates an
        // empty collision window.
        registry.patch<StaticCollider3DBindingComponent>(
            entity,
            [&entry](StaticCollider3DBindingComponent& binding) noexcept
            {
                binding.binding = entry.candidate;
            });
        registry.patch<StaticCollider3DStatusComponent>(
            entity,
            [&entry](StaticCollider3DStatusComponent& status) noexcept
            {
                status = {
                    EStaticCollider3DState::ACTIVE,
                    EStaticCollider3DFailure::NONE,
                    entry.generation};
            });
        entry.active = std::move(entry.candidate);
        entry.superseded = std::move(previous);
        entry.publish_queued = false;
        entry.state = EStaticCollider3DState::ACTIVE;
        entry.failure = EStaticCollider3DFailure::NONE;
        ++impl_->metrics.successful_publications;
    }

    void StaticCollider3DSystem::applyStatus(
        lux::ecs::Registry& registry,
        entt::entity entity,
        std::uint64_t generation,
        EStaticCollider3DState state,
        EStaticCollider3DFailure failure) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        const auto* fact = registry.valid(entity)
            ? registry.try_get<lux::ecs::StaticColliderBatch3DComponent>(
                  entity)
            : nullptr;
        const auto* transform = registry.valid(entity)
            ? registry.try_get<lux::ecs::ResolvedTransform3DComponent>(entity)
            : nullptr;
        if (found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state != state ||
            found->second.failure != failure || !fact || !transform ||
            fact->content != found->second.content ||
            !sameTransform(*transform, found->second.transform))
        {
            ++impl_->metrics.stale_publications;
            if (registry.valid(entity))
                (void)impl_->mark(entity);
            return;
        }
        auto& entry = found->second;
        if (!entry.transients_ready ||
            !registry.all_of<
                StaticCollider3DBindingComponent,
                StaticCollider3DStatusComponent>(entity))
        {
            entry.transients_ready = false;
            entry.arm_queued = false;
            entry.status_dirty = true;
            (void)impl_->mark(entity);
            return;
        }
        registry.patch<StaticCollider3DStatusComponent>(
            entity,
            [state, failure, generation](
                StaticCollider3DStatusComponent& status) noexcept
            {
                status = {state, failure, generation};
            });
    }

    void StaticCollider3DSystem::applyRemove(
        lux::ecs::Registry& registry,
        entt::entity entity,
        std::uint64_t generation) noexcept
    {
        if (!registry.valid(entity))
            return;
        if (generation != 0u &&
            registry.all_of<lux::ecs::StaticColliderBatch3DComponent,
                            lux::ecs::ResolvedTransform3DComponent>(entity))
        {
            ++impl_->metrics.stale_publications;
            (void)impl_->mark(entity);
            return;
        }
        registry.remove<StaticCollider3DBindingComponent>(entity);
        registry.remove<StaticCollider3DStatusComponent>(entity);
    }

    void StaticCollider3DSystem::acceptPreparation(
        entt::entity entity,
        std::uint64_t generation,
        lux::async::OperationOutcome<BuildStaticCollider3D> outcome) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state !=
                EStaticCollider3DState::WAITING_BACKGROUND)
        {
            if (outcome)
            {
                impl_->retireUntrackedPrepared(
                    entity, generation, std::move(*outcome));
            }
            ++impl_->metrics.stale_completions;
            return;
        }
        auto& entry = found->second;
        const bool desired_matches = impl_->desiredMatches(entry);
        if (!outcome)
        {
            if (impl_->closing)
            {
                impl_->cancelCandidate(entry);
                entry.state = EStaticCollider3DState::FAILED;
                entry.failure = EStaticCollider3DFailure::PREPARE_FAILED;
                entry.status_dirty = false;
                return;
            }
            if (outcome.error().isRuntime())
            {
                const auto error = outcome.error().runtimeError();
                if (error == lux::async::ESubmitError::QUEUE_FULL ||
                    error == lux::async::ESubmitError::
                                 BYTE_BUDGET_EXHAUSTED)
                {
                    if (!desired_matches)
                    {
                        impl_->fail(
                            entry,
                            EStaticCollider3DFailure::PREPARE_FAILED);
                        (void)impl_->mark(entity);
                        return;
                    }
                    impl_->transition(
                        entry, EStaticCollider3DState::WAITING_CONTENT);
                    ++impl_->metrics.queue_backpressure;
                    return;
                }
                impl_->fail(
                    entry, EStaticCollider3DFailure::PREPARE_FAILED);
                if (!desired_matches)
                    (void)impl_->mark(entity);
                return;
            }
            impl_->fail(
                entry,
                domainFailure(outcome.error().domainError().error));
            if (!desired_matches)
                (void)impl_->mark(entity);
            return;
        }
        if (!outcome->valid() ||
            outcome->request_generation != generation)
        {
            if (outcome->batch || outcome->budget)
            {
                impl_->retireUntrackedPrepared(
                    entity, generation, std::move(*outcome));
            }
            impl_->fail(entry, EStaticCollider3DFailure::PREPARE_FAILED);
            if (!desired_matches)
                (void)impl_->mark(entity);
            return;
        }
        entry.prepared = std::move(outcome->batch);
        entry.candidate_budget = std::move(outcome->budget);
        if (impl_->closing || !desired_matches)
        {
            impl_->cancelCandidate(entry);
            entry.state = EStaticCollider3DState::FAILED;
            entry.failure = EStaticCollider3DFailure::PREPARE_FAILED;
            entry.status_dirty = !impl_->closing;
            if (!impl_->closing)
            {
                ++impl_->metrics.stale_completions;
                (void)impl_->mark(entity);
            }
            return;
        }
        impl_->transition(entry, EStaticCollider3DState::STAGING);
    }

    void StaticCollider3DSystem::acceptPreparationStopped(
        entt::entity entity,
        std::uint64_t generation) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state !=
                EStaticCollider3DState::WAITING_BACKGROUND)
        {
            ++impl_->metrics.stale_completions;
            return;
        }
        if (impl_->closing)
        {
            impl_->cancelCandidate(found->second);
            found->second.state = EStaticCollider3DState::FAILED;
            found->second.failure =
                EStaticCollider3DFailure::PREPARE_FAILED;
            found->second.status_dirty = false;
            return;
        }
        const bool desired_matches = impl_->desiredMatches(found->second);
        impl_->fail(
            found->second, EStaticCollider3DFailure::PREPARE_FAILED);
        if (!desired_matches)
            (void)impl_->mark(entity);
    }

    StaticCollider3DSystemSnapshot
    StaticCollider3DSystem::snapshot() const noexcept
    {
        auto result = impl_->metrics;
        result.tracked_entities = static_cast<std::uint32_t>(
            impl_->entries.size());
        result.waiting_entities = 0u;
        result.background_entities = 0u;
        result.staging_entities = 0u;
        result.ready_entities = 0u;
        result.active_entities = 0u;
        result.failed_entities = 0u;
        result.retirement_queue_size = static_cast<std::uint32_t>(
            impl_->retirement.size());
        result.closing = impl_->closing;
        for (const auto& [_, entry] : impl_->entries)
        {
            result.owned_budget_bytes += entry.candidate_budget.accountedBytes();
            if (entry.candidate)
                result.owned_budget_bytes += entry.candidate->accountedBytes();
            if (entry.active)
                result.owned_budget_bytes += entry.active->accountedBytes();
            if (entry.superseded)
            {
                result.owned_budget_bytes +=
                    entry.superseded->accountedBytes();
            }
            if (entry.active && entry.active->active())
                ++result.active_entities;
            switch (entry.state)
            {
            case EStaticCollider3DState::WAITING_CONTENT:
                ++result.waiting_entities;
                break;
            case EStaticCollider3DState::WAITING_BACKGROUND:
                ++result.background_entities;
                break;
            case EStaticCollider3DState::STAGING:
                ++result.staging_entities;
                break;
            case EStaticCollider3DState::READY:
                ++result.ready_entities;
                break;
            case EStaticCollider3DState::ACTIVE:
                break;
            case EStaticCollider3DState::FAILED:
                ++result.failed_entities;
                break;
            }
        }
        for (const auto& binding : impl_->retirement)
        {
            if (binding)
            {
                result.owned_budget_bytes += binding->accountedBytes();
                result.retirement_body_count += binding->remainingBodies();
                result.retirement_unit_count +=
                    binding->remainingRetirementUnits();
            }
        }
        result.closed = closeComplete();
        return result;
    }

    std::optional<StaticCollider3DStatusComponent>
    StaticCollider3DSystem::status(entt::entity entity) const noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (found == impl_->entries.end() ||
            found->second.entity != entity)
        {
            return std::nullopt;
        }
        return StaticCollider3DStatusComponent{
            found->second.state,
            found->second.failure,
            found->second.generation};
    }

    void StaticCollider3DSystem::requestClose() noexcept
    {
        if (impl_->closing)
            return;
        impl_->closing = true;
        for (auto& [_, entry] : impl_->entries)
        {
            if (entry.state == EStaticCollider3DState::WAITING_BACKGROUND)
            {
                // The accepted operation cannot be synchronously canceled.
                // Keep its ContentBlob lease until the terminal callback can
                // transfer a successful preparation into bounded retirement.
                impl_->retirePublished(entry);
            }
            else
            {
                impl_->cancelCandidate(entry);
                impl_->retirePublished(entry);
            }
            entry.status_dirty = false;
        }
        impl_->dirty.clear();
        impl_->dirty_scratch.clear();
        impl_->rescan_required = false;
    }

    bool StaticCollider3DSystem::closeComplete() const noexcept
    {
        if (!impl_->closing || !impl_->retirement.empty() ||
            impl_->hasTransientComponents())
            return false;
        return std::ranges::none_of(
            impl_->entries,
            [](const auto& value) noexcept
            {
                const auto& entry = value.second;
                return entry.state ==
                        EStaticCollider3DState::WAITING_BACKGROUND ||
                    entry.prepared || entry.stager || entry.candidate ||
                    entry.active || entry.superseded ||
                    static_cast<bool>(entry.candidate_budget);
            });
    }

    bool StaticCollider3DSystem::closeNeedsOwnerTick() const noexcept
    {
        if (!impl_->closing || closeComplete())
            return false;
        if (!impl_->retirement.empty() || impl_->hasTransientComponents())
            return true;
        return std::ranges::any_of(
            impl_->entries,
            [](const auto& value) noexcept
            {
                const auto& entry = value.second;
                return entry.state !=
                        EStaticCollider3DState::WAITING_BACKGROUND &&
                    (entry.prepared || entry.stager || entry.candidate ||
                     entry.active || entry.superseded ||
                     static_cast<bool>(entry.candidate_budget));
            });
    }
} // namespace lux::runtime::spatial3d
