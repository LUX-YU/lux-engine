#include <lux/engine/runtime/spatial2d/infinite/Infinite2DPixelSystem.hpp>

#include <lux/engine/ecs/pixel/components/PixelChunk2DComponent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>
#include <lux/engine/ecs/pixel/systems/PixelChunkPersistence.hpp>
#include <lux/engine/runtime/spatial2d/infinite/Infinite2DPixelContent.hpp>
#include <lux/engine/runtime/spatial2d/infinite/PixelChunkBindingComponent.hpp>
#include <lux/engine/runtime/spatial2d/infinite/SpatialInterest2DSystem.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <utility>
#include <vector>

namespace lux::runtime::spatial2d
{
    namespace
    {
        enum class EOwnedChunkState : std::uint8_t
        {
            FREE,
            WAITING_ADMISSION,
            WAITING_BACKGROUND,
            STAGING,
            PUBLISHED,
            RETIRE_REQUESTED,
            RETIRING
        };

        struct OwnedChunk final
        {
            std::uint32_t generation{1u};
            EOwnedChunkState state{EOwnedChunkState::FREE};
            entt::entity entity{entt::null};
            lux::ecs::PixelFieldHandle field;
            lux::math::GridCoord2i64 coordinate;
            lux::ecs::PersistentEntityRef field_reference;
            lux::ecs::scene_format::ContentBlobRef content_reference;
            lux::ecs::entity_scene::ContentBlobLease content;
            bool active{false};
            bool adopted{false};
            EPixelChunkDomainError prepare_error{
                EPixelChunkDomainError::NONE};
        };

        struct CompletionControl final
        {
            std::atomic<Infinite2DPixelSystem*> owner{nullptr};
        };
    }

    struct Infinite2DPixelSystem::Impl final
    {
        Impl(
            Infinite2DPixelSystem& owner_value,
            lux::exec::AsyncRuntime& async_runtime_value,
            Infinite2DPixelPrepareClient preparation_value,
            lux::ecs::PixelFieldRuntime& runtime_value,
            lux::ecs::PixelFieldSystem& fields_value,
            lux::ecs::PixelChunkPersistenceStore& persistence_value,
            lux::ecs::entity_scene::ContentBlobClient content_value,
            const SpatialInterest2DSystem& activity_value) noexcept
            : async_runtime(&async_runtime_value),
              preparation(std::move(preparation_value)),
              scope(async_runtime_value),
              completion(std::make_shared<CompletionControl>()),
              runtime(&runtime_value),
              fields(&fields_value),
              persistence(&persistence_value),
              content(std::move(content_value)),
              activity(&activity_value)
        {
            completion->owner.store(&owner_value, std::memory_order_release);
        }

        [[nodiscard]] OwnedChunk* find(entt::entity entity) noexcept
        {
            const auto found = std::find_if(
                chunks.begin(), chunks.end(),
                [entity](const OwnedChunk& chunk)
                {
                    return chunk.state != EOwnedChunkState::FREE &&
                        chunk.entity == entity;
                });
            return found == chunks.end() ? nullptr : &*found;
        }

        [[nodiscard]] OwnedChunk* find(
            std::uint32_t slot,
            std::uint32_t generation) noexcept
        {
            if (slot >= chunks.size())
                return nullptr;
            auto& chunk = chunks[slot];
            return chunk.state != EOwnedChunkState::FREE &&
                chunk.generation == generation
                ? &chunk
                : nullptr;
        }

        [[nodiscard]] std::uint32_t slotOf(
            const OwnedChunk& chunk) const noexcept
        {
            return static_cast<std::uint32_t>(&chunk - chunks.data());
        }

        [[nodiscard]] OwnedChunk& allocate()
        {
            const auto free = std::find_if(
                chunks.begin(), chunks.end(),
                [](const OwnedChunk& chunk)
                {
                    return chunk.state == EOwnedChunkState::FREE &&
                        chunk.generation != 0u;
                });
            if (free != chunks.end())
                return *free;
            chunks.emplace_back();
            return chunks.back();
        }

        [[nodiscard]] bool enqueue(Infinite2DPixelCommand command) noexcept
        {
            if (commands.push(command))
                return true;
            ++snapshot.command_rejections;
            return false;
        }

        void launch(OwnedChunk& chunk) noexcept
        {
            if (chunk.state != EOwnedChunkState::WAITING_ADMISSION)
                return;
            if (!preparation)
            {
                chunk.prepare_error =
                    EPixelChunkDomainError::CONTENT_UNAVAILABLE;
                chunk.state = EOwnedChunkState::STAGING;
                static_cast<void>(enqueue(Infinite2DPixelCommand{
                    EInfinite2DPixelCommandAction::PUBLISH,
                    chunk.entity,
                    slotOf(chunk),
                    chunk.generation}));
                return;
            }
            const auto slot = slotOf(chunk);
            const auto generation = chunk.generation;
            auto preparation_context =
                runtime->chunkPreparationContext(chunk.field);
            if (!preparation_context)
            {
                chunk.prepare_error =
                    EPixelChunkDomainError::RUNTIME_REJECTED;
                chunk.state = EOwnedChunkState::STAGING;
                static_cast<void>(enqueue(Infinite2DPixelCommand{
                    EInfinite2DPixelCommandAction::PUBLISH,
                    chunk.entity,
                    slot,
                    generation}));
                return;
            }
            std::optional<lux::ecs::scene_format::PersistenceJournalRecord>
                persistence_record;
            if (const auto* record = persistence->latest(chunk.coordinate))
                persistence_record.emplace(*record);
            const auto persistence_bytes = persistence_record
                ? persistence_record->record_bytes.size()
                : 0u;
            auto pipeline = lux::exec::execute(
                    preparation.operation(),
                    PrepareInfinite2DPixelChunk{
                        chunk.content.bytes(),
                        chunk.content_reference,
                        chunk.coordinate,
                        std::move(*preparation_context),
                        std::move(persistence_record),
                        generation},
                    lux::async::SubmitOptions{
                        .accounted_bytes =
                            (sizeof(lux::ecs::MaterialId) +
                             sizeof(std::uint8_t)) *
                            lux::ecs::PixelFieldRuntime::kChunkCellCount +
                            persistence_bytes})
                | stdexec::continues_on(
                      lux::exec::mainThreadScheduler(*async_runtime))
                | stdexec::then(
                      [weak = std::weak_ptr{completion}, slot, generation](
                          lux::async::OperationOutcome<
                              PrepareInfinite2DPixelChunk> outcome)
                          mutable noexcept
                      {
                          const auto locked = weak.lock();
                          if (!locked)
                              return;
                          auto* target = locked->owner.load(
                              std::memory_order_acquire);
                          if (target)
                          {
                              target->acceptPreparation(
                                  slot,
                                  generation,
                                  std::move(outcome));
                          }
                      })
                | stdexec::upon_stopped(
                      [weak = std::weak_ptr{completion}, slot, generation]()
                          noexcept
                      {
                          const auto locked = weak.lock();
                          if (!locked)
                              return;
                          auto* target = locked->owner.load(
                              std::memory_order_acquire);
                          if (target)
                          {
                              target->acceptPreparationStopped(
                                  slot, generation);
                          }
                      });
            chunk.state = EOwnedChunkState::WAITING_BACKGROUND;
            if (snapshot.admission_chunks != 0u)
                --snapshot.admission_chunks;
            ++snapshot.background_chunks;
            if (!lux::exec::spawn(scope, std::move(pipeline)))
            {
                chunk.state = EOwnedChunkState::WAITING_ADMISSION;
                --snapshot.background_chunks;
                ++snapshot.admission_chunks;
                ++snapshot.queue_backpressure;
            }
        }

        void enqueueReconcile(entt::entity entity) noexcept
        {
            if (closing)
                return;
            static_cast<void>(enqueue(Infinite2DPixelCommand{
                EInfinite2DPixelCommandAction::RECONCILE,
                entity}));
        }

        void enqueueCloseFence() noexcept
        {
            if (!closing || close_fence_queued || close_fence_applied)
                return;
            if (enqueue(Infinite2DPixelCommand{
                    EInfinite2DPixelCommandAction::CLOSE_FENCE}))
            {
                close_fence_queued = true;
            }
        }

        void hide(OwnedChunk& chunk) noexcept
        {
            if (!chunk.active)
                return;
            static_cast<void>(runtime->setChunkPresentationActive(
                chunk.field, chunk.coordinate, false));
            static_cast<void>(runtime->setChunkSimulationActive(
                chunk.field, chunk.coordinate, false));
            chunk.active = false;
            ++snapshot.hidden_chunks;
        }

        void requestRetire(OwnedChunk& chunk) noexcept
        {
            if (chunk.state == EOwnedChunkState::RETIRE_REQUESTED ||
                chunk.state == EOwnedChunkState::RETIRING ||
                chunk.state == EOwnedChunkState::FREE)
            {
                return;
            }
            if (enqueue(Infinite2DPixelCommand{
                EInfinite2DPixelCommandAction::RETIRE,
                entt::null,
                slotOf(chunk),
                chunk.generation}))
            {
                chunk.state = EOwnedChunkState::RETIRE_REQUESTED;
            }
        }

        void onChunkConstructed(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            enqueueReconcile(entity);
        }

        void onChunkUpdated(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            enqueueReconcile(entity);
        }

        void onChunkDestroyed(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            if (auto* chunk = find(entity))
                requestRetire(*chunk);
        }

        void onBindingDestroyed(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            const auto* binding =
                registry.try_get<PixelChunkBindingComponent>(entity);
            if (!binding)
                return;
            if (auto* chunk = find(binding->slot, binding->generation))
                requestRetire(*chunk);
        }

        void attach(
            lux::ecs::Registry& registry,
            lux::ecs::EcsCommandWriter writer)
        {
            if (attached)
                std::abort();
            attached = &registry;
            commands = writer;
            constructed = registry.on_construct<
                lux::ecs::PixelChunk2DComponent>()
                .connect<&Impl::onChunkConstructed>(*this);
            updated = registry.on_update<
                lux::ecs::PixelChunk2DComponent>()
                .connect<&Impl::onChunkUpdated>(*this);
            destroyed = registry.on_destroy<
                lux::ecs::PixelChunk2DComponent>()
                .connect<&Impl::onChunkDestroyed>(*this);
            binding_destroyed = registry.on_destroy<
                PixelChunkBindingComponent>()
                .connect<&Impl::onBindingDestroyed>(*this);

            for (const auto entity :
                 registry.view<const lux::ecs::PixelChunk2DComponent>())
            {
                enqueueReconcile(entity);
            }
        }

        void setStatus(
            lux::ecs::Registry& registry,
            entt::entity entity,
            EPixelChunkDomainState state,
            EPixelChunkDomainError error)
        {
            if (!registry.valid(entity) ||
                !registry.all_of<lux::ecs::PixelChunk2DComponent>(entity))
            {
                return;
            }
            registry.emplace_or_replace<PixelChunkDomainStateComponent>(
                entity,
                PixelChunkDomainStateComponent{state, error});
        }

        void fail(
            lux::ecs::Registry& registry,
            entt::entity entity,
            EPixelChunkDomainError error)
        {
            setStatus(
                registry, entity, EPixelChunkDomainState::FAILED, error);
        }

        void release(OwnedChunk& chunk) noexcept
        {
            chunk.content = {};
            chunk.entity = entt::null;
            chunk.field = {};
            chunk.coordinate = {};
            chunk.field_reference = {};
            chunk.content_reference = {};
            chunk.active = false;
            chunk.adopted = false;
            chunk.prepare_error = EPixelChunkDomainError::NONE;
            chunk.state = EOwnedChunkState::FREE;
            ++chunk.generation;
            // A wrapped generation can never safely identify a reused slot.
            if (chunk.generation == 0u)
                chunk.generation = 0u;
            ++snapshot.retired_chunks;
        }

        void recount(lux::ecs::Registry& registry) noexcept
        {
            snapshot.waiting_chunks = 0u;
            snapshot.staging_chunks = 0u;
            snapshot.ready_chunks = 0u;
            snapshot.failed_chunks = 0u;
            snapshot.resident_chunks = 0u;
            snapshot.active_chunks = 0u;
            snapshot.admission_chunks = 0u;
            snapshot.background_chunks = 0u;
            snapshot.publish_pending_chunks = 0u;
            snapshot.retiring_chunks = 0u;
            registry.view<const PixelChunkDomainStateComponent>().each(
                [this](const PixelChunkDomainStateComponent& status)
                {
                    switch (status.state)
                    {
                    case EPixelChunkDomainState::WAITING_FIELD:
                        ++snapshot.waiting_chunks;
                        break;
                    case EPixelChunkDomainState::STAGING:
                        ++snapshot.staging_chunks;
                        break;
                    case EPixelChunkDomainState::READY:
                        ++snapshot.ready_chunks;
                        break;
                    case EPixelChunkDomainState::FAILED:
                        ++snapshot.failed_chunks;
                        break;
                    }
                });
            for (const auto& chunk : chunks)
            {
                if (chunk.state == EOwnedChunkState::FREE)
                    continue;
                ++snapshot.resident_chunks;
                if (chunk.active)
                    ++snapshot.active_chunks;
                switch (chunk.state)
                {
                case EOwnedChunkState::WAITING_ADMISSION:
                    ++snapshot.admission_chunks;
                    break;
                case EOwnedChunkState::WAITING_BACKGROUND:
                    ++snapshot.background_chunks;
                    break;
                case EOwnedChunkState::STAGING:
                    ++snapshot.publish_pending_chunks;
                    break;
                case EOwnedChunkState::RETIRING:
                    ++snapshot.retiring_chunks;
                    break;
                case EOwnedChunkState::RETIRE_REQUESTED:
                    ++snapshot.retiring_chunks;
                    break;
                case EOwnedChunkState::PUBLISHED:
                case EOwnedChunkState::FREE:
                    break;
                }
            }
            snapshot.scope_closed = scope_closed;
            snapshot.closed = closing && scope_closed &&
                close_fence_applied &&
                std::all_of(
                    chunks.begin(),
                    chunks.end(),
                    [](const OwnedChunk& chunk)
                    {
                        return chunk.state == EOwnedChunkState::FREE;
                    });
        }

        lux::exec::AsyncRuntime* async_runtime{nullptr};
        Infinite2DPixelPrepareClient preparation;
        lux::exec::AsyncScope scope;
        std::shared_ptr<CompletionControl> completion;
        lux::ecs::PixelFieldRuntime* runtime{nullptr};
        lux::ecs::PixelFieldSystem* fields{nullptr};
        lux::ecs::PixelChunkPersistenceStore* persistence{nullptr};
        lux::ecs::entity_scene::ContentBlobClient content;
        const SpatialInterest2DSystem* activity{nullptr};
        lux::ecs::Registry* attached{nullptr};
        lux::ecs::EcsCommandWriter commands;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        entt::scoped_connection binding_destroyed;
        std::vector<OwnedChunk> chunks;
        Infinite2DPixelSnapshot snapshot;
        bool closing{false};
        bool scope_close_started{false};
        bool scope_closed{false};
        bool close_fence_queued{false};
        bool close_fence_applied{false};
        lux::ecs::SystemCloseProgressSink close_progress;
    };

    Infinite2DPixelSystem::Infinite2DPixelSystem(
        lux::exec::AsyncRuntime& async_runtime,
        Infinite2DPixelPrepareClient preparation,
        lux::ecs::PixelFieldRuntime& runtime,
        lux::ecs::PixelFieldSystem& fields,
        lux::ecs::PixelChunkPersistenceStore& persistence,
        lux::ecs::entity_scene::ContentBlobClient content,
        const SpatialInterest2DSystem& activity)
        : impl_(std::make_unique<Impl>(
              *this,
              async_runtime,
              std::move(preparation),
              runtime,
              fields,
              persistence,
              std::move(content),
              activity))
    {}

    Infinite2DPixelSystem::~Infinite2DPixelSystem()
    {
        impl_->completion->owner.store(nullptr, std::memory_order_release);
    }

    Infinite2DPixelSnapshot Infinite2DPixelSystem::snapshot() const noexcept
    {
        return impl_->snapshot;
    }

    void Infinite2DPixelSystem::requestClose() noexcept
    {
        requestClose({});
    }

    void Infinite2DPixelSystem::requestClose(
        lux::ecs::SystemCloseProgressSink progress) noexcept
    {
        if (progress)
            impl_->close_progress = progress;
        if (!impl_->closing)
        {
            impl_->closing = true;
            impl_->snapshot.closing = true;
            for (auto& chunk : impl_->chunks)
                impl_->requestRetire(chunk);
            impl_->enqueueCloseFence();
        }
        if (!impl_->scope_close_started)
        {
            impl_->scope_close_started = true;
            lux::exec::detail::subscribeScopeClose(
                impl_->scope,
                [weak = std::weak_ptr{impl_->completion}]() noexcept
                {
                    const auto locked = weak.lock();
                    if (!locked)
                        return;
                    if (auto* owner = locked->owner.load(
                            std::memory_order_acquire))
                    {
                        owner->acceptScopeClosed();
                    }
                });
        }
    }

    bool Infinite2DPixelSystem::closeComplete() const noexcept
    {
        return impl_->closing && impl_->scope_closed &&
            impl_->close_fence_applied &&
            std::all_of(
                impl_->chunks.begin(),
                impl_->chunks.end(),
                [](const OwnedChunk& chunk)
                {
                    return chunk.state == EOwnedChunkState::FREE;
                });
    }

    bool Infinite2DPixelSystem::closeNeedsOwnerTick() const noexcept
    {
        if (!impl_->closing || closeComplete())
            return false;
        if (!impl_->close_fence_applied)
            return true;
        // A background preparation is external progress. Its scene-local
        // scope terminal callback wakes close after every completion has been
        // adopted; re-ticking the owner before then cannot change the chunk.
        return std::any_of(
            impl_->chunks.begin(),
            impl_->chunks.end(),
            [](const OwnedChunk& chunk)
            {
                return chunk.state != EOwnedChunkState::FREE &&
                    chunk.state !=
                        EOwnedChunkState::WAITING_BACKGROUND;
            });
    }

    lux::exec::AsyncScopeCloseSender
    Infinite2DPixelSystem::closeAsync() noexcept
    {
        requestClose();
        return impl_->scope.closeAsync();
    }

    void Infinite2DPixelSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void Infinite2DPixelSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        if (impl_->attached != &context.registry())
            std::abort();
        auto& registry = context.registry();
        impl_->enqueueCloseFence();
        impl_->snapshot.retirement_granules_last_update = 0u;
        for (auto& chunk : impl_->chunks)
        {
            if (chunk.state != EOwnedChunkState::FREE &&
                chunk.state != EOwnedChunkState::RETIRING &&
                chunk.state != EOwnedChunkState::RETIRE_REQUESTED &&
                (impl_->closing || !registry.valid(chunk.entity) ||
                 !registry.all_of<lux::ecs::PixelChunk2DComponent>(
                     chunk.entity)))
            {
                impl_->requestRetire(chunk);
            }
        }
        retireOne();
        for (auto& chunk : impl_->chunks)
        {
            if (!impl_->closing &&
                chunk.state == EOwnedChunkState::WAITING_ADMISSION)
            {
                impl_->launch(chunk);
            }
            if (!impl_->closing &&
                chunk.state == EOwnedChunkState::STAGING)
            {
                static_cast<void>(impl_->enqueue(Infinite2DPixelCommand{
                    EInfinite2DPixelCommandAction::PUBLISH,
                    chunk.entity,
                    impl_->slotOf(chunk),
                    chunk.generation}));
            }
        }
        registry.view<const lux::ecs::PixelChunk2DComponent>().each(
            [this, &registry](
                entt::entity entity,
                const lux::ecs::PixelChunk2DComponent& component)
            {
                auto* chunk = impl_->find(entity);
                if (!chunk)
                {
                    if (impl_->closing)
                        return;
                    if (registry.all_of<PixelChunkDomainStateComponent>(
                            entity))
                    {
                        ++impl_->snapshot.retries;
                    }
                    impl_->enqueueReconcile(entity);
                    return;
                }
                if (chunk->coordinate != component.coordinate ||
                    chunk->field_reference != component.field ||
                    chunk->content_reference != component.content)
                {
                    // Observer enqueue is bounded.  A rejected edge must be
                    // recoverable from authoritative ECS facts on a later
                    // update rather than silently losing the component patch.
                    // Mark the old owner for retirement here as well: this is
                    // the owner phase, so it is safe to update private state,
                    // and it makes the deferred barrier follow-up observable
                    // without waiting for an unrelated async mailbox epoch.
                    impl_->requestRetire(*chunk);
                    impl_->enqueueReconcile(entity);
                    return;
                }
                if (chunk->state != EOwnedChunkState::PUBLISHED)
                    return;
                const bool active = impl_->activity->isActive(
                    component.coordinate);
                if (active == chunk->active)
                    return;
                const bool presentation =
                    impl_->runtime->setChunkPresentationActive(
                        chunk->field, chunk->coordinate, active);
                const bool simulation =
                    impl_->runtime->setChunkSimulationActive(
                        chunk->field, chunk->coordinate, active);
                if (presentation && simulation)
                {
                    chunk->active = active;
                    if (!active)
                        ++impl_->snapshot.hidden_chunks;
                }
            });
        if (context.dt() > 0.0f)
            impl_->runtime->step();
        impl_->recount(registry);
    }

    void Infinite2DPixelSystem::applyReconcile(
        lux::ecs::Registry& registry,
        entt::entity entity) noexcept
    {
        if (impl_->attached != &registry)
            std::abort();
        if (impl_->closing)
        {
            if (auto* existing = impl_->find(entity))
                impl_->requestRetire(*existing);
            return;
        }
        const auto* component =
            registry.try_get<lux::ecs::PixelChunk2DComponent>(entity);
        if (!component)
        {
            if (auto* existing = impl_->find(entity))
                impl_->requestRetire(*existing);
            return;
        }
        if (auto* existing = impl_->find(entity))
        {
            if (existing->coordinate == component->coordinate &&
                existing->field_reference == component->field &&
                existing->content_reference == component->content)
            {
                return;
            }
            impl_->requestRetire(*existing);
            impl_->enqueueReconcile(entity);
            return;
        }

        const auto field = impl_->fields->resolveField(component->field);
        if (!field.isValid())
        {
            impl_->setStatus(
                registry,
                entity,
                EPixelChunkDomainState::WAITING_FIELD,
                EPixelChunkDomainError::NONE);
            return;
        }
        auto content = impl_->content.resolve(component->content);
        if (!content)
        {
            impl_->fail(
                registry,
                entity,
                EPixelChunkDomainError::CONTENT_UNAVAILABLE);
            return;
        }
        auto& prepared = impl_->allocate();
        prepared.state = EOwnedChunkState::WAITING_ADMISSION;
        prepared.entity = entity;
        prepared.field = field;
        prepared.coordinate = component->coordinate;
        prepared.field_reference = component->field;
        prepared.content_reference = component->content;
        prepared.content = std::move(*content);
        prepared.active = false;
        prepared.adopted = false;
        impl_->setStatus(
            registry,
            entity,
            EPixelChunkDomainState::STAGING,
            EPixelChunkDomainError::NONE);
        impl_->launch(prepared);
    }

    void Infinite2DPixelSystem::applyPublish(
        lux::ecs::Registry& registry,
        entt::entity entity,
        std::uint32_t slot,
        std::uint32_t generation) noexcept
    {
        auto* prepared = impl_->find(slot, generation);
        const auto* component =
            registry.try_get<lux::ecs::PixelChunk2DComponent>(entity);
        if (!prepared || prepared->state != EOwnedChunkState::STAGING)
            return;
        if (prepared->prepare_error != EPixelChunkDomainError::NONE)
        {
            const auto error = prepared->prepare_error;
            impl_->fail(registry, entity, error);
            impl_->requestRetire(*prepared);
            return;
        }
        if (!component || prepared->entity != entity ||
            prepared->coordinate != component->coordinate ||
            prepared->field_reference != component->field ||
            prepared->content_reference != component->content)
        {
            impl_->requestRetire(*prepared);
            return;
        }

        const bool active = impl_->activity->isActive(
            prepared->coordinate);
        const bool presentation =
            impl_->runtime->setChunkPresentationActive(
                prepared->field, prepared->coordinate, active);
        const bool simulation = impl_->runtime->setChunkSimulationActive(
            prepared->field, prepared->coordinate, active);
        if (!presentation || !simulation)
        {
            impl_->requestRetire(*prepared);
            impl_->fail(
                registry,
                entity,
                EPixelChunkDomainError::RUNTIME_REJECTED);
            return;
        }
        prepared->active = active;
        prepared->state = EOwnedChunkState::PUBLISHED;
        registry.emplace_or_replace<PixelChunkBindingComponent>(
            entity,
            PixelChunkBindingComponent{slot, generation});
        impl_->setStatus(
            registry,
            entity,
            EPixelChunkDomainState::READY,
            EPixelChunkDomainError::NONE);
        ++impl_->snapshot.published_chunks;
    }

    void Infinite2DPixelSystem::acceptPreparation(
        std::uint32_t slot,
        std::uint32_t generation,
        lux::async::OperationOutcome<PrepareInfinite2DPixelChunk> outcome)
        noexcept
    {
        auto* prepared = impl_->find(slot, generation);
        if (!prepared ||
            prepared->state != EOwnedChunkState::WAITING_BACKGROUND)
        {
            ++impl_->snapshot.stale_completions;
            return;
        }
        if (!outcome)
        {
            if (outcome.error().isRuntime())
            {
                const auto error = outcome.error().runtimeError();
                if (error == lux::async::ESubmitError::QUEUE_FULL ||
                    error == lux::async::ESubmitError::
                        BYTE_BUDGET_EXHAUSTED)
                {
                    prepared->state =
                        EOwnedChunkState::WAITING_ADMISSION;
                    ++impl_->snapshot.queue_backpressure;
                    return;
                }
            }
            prepared->prepare_error =
                EPixelChunkDomainError::CONTENT_INVALID;
            prepared->state = EOwnedChunkState::STAGING;
        }
        else if (outcome->request_generation != generation ||
                 outcome->chunk.coordinate() != prepared->coordinate)
        {
            ++impl_->snapshot.stale_completions;
            prepared->prepare_error =
                EPixelChunkDomainError::CONTENT_INVALID;
            prepared->state = EOwnedChunkState::STAGING;
        }
        else
        {
            if (impl_->persistence->latest(prepared->coordinate))
                ++impl_->snapshot.persistence_recoveries;
            if (!impl_->runtime->adoptPreparedChunk(
                    prepared->field, std::move(outcome->chunk)))
            {
                prepared->prepare_error =
                    EPixelChunkDomainError::RUNTIME_REJECTED;
            }
            else
            {
                prepared->adopted = true;
                ++impl_->snapshot.prepared_chunks;
            }
            prepared->state = EOwnedChunkState::STAGING;
        }

        if (!impl_->enqueue(Infinite2DPixelCommand{
                EInfinite2DPixelCommandAction::PUBLISH,
                prepared->entity,
                slot,
                generation}))
        {
            prepared->state = EOwnedChunkState::RETIRING;
        }
    }

    void Infinite2DPixelSystem::acceptPreparationStopped(
        std::uint32_t slot,
        std::uint32_t generation) noexcept
    {
        auto* prepared = impl_->find(slot, generation);
        if (!prepared ||
            prepared->state != EOwnedChunkState::WAITING_BACKGROUND)
        {
            ++impl_->snapshot.stale_completions;
            return;
        }
        prepared->prepare_error =
            EPixelChunkDomainError::CONTENT_UNAVAILABLE;
        prepared->state = EOwnedChunkState::STAGING;
        static_cast<void>(impl_->enqueue(Infinite2DPixelCommand{
            EInfinite2DPixelCommandAction::PUBLISH,
            prepared->entity,
            slot,
            generation}));
    }

    void Infinite2DPixelSystem::acceptScopeClosed() noexcept
    {
        impl_->scope_closed = true;
        impl_->snapshot.scope_closed = true;
        impl_->snapshot.closed = closeComplete();
        if (impl_->close_progress)
            impl_->close_progress.notify();
    }

    void Infinite2DPixelSystem::applyCloseFence() noexcept
    {
        impl_->close_fence_queued = false;
        impl_->close_fence_applied = true;
        impl_->snapshot.closed = closeComplete();
    }

    void Infinite2DPixelSystem::applyRetire(
        lux::ecs::Registry& registry,
        std::uint32_t slot,
        std::uint32_t generation) noexcept
    {
        auto* chunk = impl_->find(slot, generation);
        if (!chunk || chunk->state != EOwnedChunkState::RETIRE_REQUESTED)
            return;
        chunk->state = EOwnedChunkState::RETIRING;
        impl_->hide(*chunk);
        const auto entity = chunk->entity;
        if (registry.valid(entity))
        {
            const auto* binding =
                registry.try_get<PixelChunkBindingComponent>(entity);
            if (binding && binding->slot == slot &&
                binding->generation == generation)
            {
                registry.remove<PixelChunkBindingComponent>(entity);
            }
            registry.remove<PixelChunkDomainStateComponent>(entity);
        }
        ++impl_->snapshot.retirement_barriers;
    }

    void Infinite2DPixelSystem::retireOne() noexcept
    {
        const auto found = std::find_if(
            impl_->chunks.begin(),
            impl_->chunks.end(),
            [](const OwnedChunk& chunk)
            {
                return chunk.state == EOwnedChunkState::RETIRING;
            });
        if (found == impl_->chunks.end())
            return;
        if (found->adopted &&
            impl_->runtime->isAlive(found->field))
        {
            auto captured = impl_->persistence->capture(
                *impl_->runtime,
                found->field,
                found->coordinate);
            if (!captured)
                ++impl_->snapshot.persistence_failures;
            if (!impl_->runtime->discardChunk(
                    found->field, found->coordinate))
            {
                return;
            }
        }
        impl_->release(*found);
        ++impl_->snapshot.retirement_granules;
        ++impl_->snapshot.retirement_granules_last_update;
        impl_->snapshot.maximum_retirement_granules_per_update = std::max(
            impl_->snapshot.maximum_retirement_granules_per_update,
            impl_->snapshot.retirement_granules_last_update);
    }

    std::span<const lux::ecs::ISystem::Type>
    Infinite2DPixelSystem::prerequisites() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<lux::ecs::PixelFieldSystem>(),
            lux::ecs::systemType<SpatialInterest2DSystem>()};
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    Infinite2DPixelSystem::runsAfter() const noexcept
    {
        return prerequisites();
    }

    void Infinite2DPixelCommand::prepareRegistryPublication(
        lux::ecs::Registry& registry) const noexcept
    {
        lux::ecs::reserveEcsCommandStorage(
            registry.storage<PixelChunkDomainStateComponent>(), 1u);
        lux::ecs::reserveEcsCommandStorage(
            registry.storage<PixelChunkBindingComponent>(), 1u);
    }

    void Infinite2DPixelCommand::apply(
        lux::ecs::Registry& registry,
        Infinite2DPixelSystem& system) const noexcept
    {
        switch (action)
        {
        case EInfinite2DPixelCommandAction::RECONCILE:
            system.applyReconcile(registry, entity);
            break;
        case EInfinite2DPixelCommandAction::PUBLISH:
            system.applyPublish(registry, entity, slot, generation);
            break;
        case EInfinite2DPixelCommandAction::RETIRE:
            system.applyRetire(registry, slot, generation);
            break;
        case EInfinite2DPixelCommandAction::CLOSE_FENCE:
            system.applyCloseFence();
            break;
        }
    }
}
