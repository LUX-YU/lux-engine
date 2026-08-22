#include <lux/engine/runtime/spatial2d/tilemap/TilemapChunkSystem.hpp>

#include <lux/engine/ecs/tilemap/components/TileChunk2DComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapSystem.hpp>
#include <lux/engine/ecs/tilemap/TilemapChunkCodec.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapChunkBindingComponent.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace lux::runtime::spatial2d
{
    namespace
    {
        enum class EOwnedState : std::uint8_t
        {
            FREE,
            WAITING_ADMISSION,
            WAITING_BACKGROUND,
            READY_TO_PUBLISH,
            PUBLISHED,
            FAILED,
            RETIRING
        };

        struct OwnedChunk final
        {
            std::uint32_t generation{1u};
            EOwnedState state{EOwnedState::FREE};
            entt::entity entity{entt::null};
            lux::ecs::TilemapHandle tilemap;
            lux::ecs::TileChunkCoord coordinate;
            lux::ecs::PersistentEntityRef tilemap_reference;
            lux::ecs::scene_format::ContentBlobRef content_reference;
            lux::runtime::entity_scene::ContentBlobLease content;
            std::optional<lux::ecs::TileChunkLoad> prepared;
            ETilemapChunkDomainError prepare_error{
                ETilemapChunkDomainError::NONE};
            bool active{false};
            bool adopted{false};
            bool retire_requested{false};
        };

        struct CompletionControl final
        {
            std::atomic<TilemapChunkSystem*> owner{nullptr};
        };

        [[nodiscard]] bool sameFact(
            const OwnedChunk& owned,
            const lux::ecs::TileChunk2DComponent& fact) noexcept
        {
            return owned.coordinate == fact.coordinate &&
                owned.tilemap_reference == fact.tilemap &&
                owned.content_reference == fact.content;
        }
    } // namespace

    struct TilemapChunkSystem::Impl final
    {
        Impl(
            TilemapChunkSystem& owner,
            lux::exec::AsyncRuntime& async_runtime_value,
            lux::exec::AsyncScope& scene_scope_value,
            TilemapPrepareClient preparation_value,
            lux::ecs::TilemapRuntime& runtime_value,
            lux::ecs::TilemapSystem& tilemaps_value,
            lux::runtime::entity_scene::ContentBlobClient content_value,
            const TilemapChunkActivity2D* activity_value,
            TilemapChunkSystemConfig config_value) noexcept
            : async_runtime(&async_runtime_value),
              scene_scope(&scene_scope_value),
              preparation(std::move(preparation_value)),
              runtime(&runtime_value),
              tilemaps(&tilemaps_value),
              content(std::move(content_value)),
              activity(activity_value),
              config(config_value),
              completion(std::make_shared<CompletionControl>())
        {
            completion->owner.store(&owner, std::memory_order_release);
        }

        [[nodiscard]] OwnedChunk* find(entt::entity entity) noexcept
        {
            const auto found = std::find_if(
                chunks.begin(), chunks.end(),
                [entity](const OwnedChunk& chunk) noexcept
                {
                    return chunk.state != EOwnedState::FREE &&
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
            return chunk.state != EOwnedState::FREE &&
                chunk.generation == generation
                ? &chunk
                : nullptr;
        }

        [[nodiscard]] std::uint32_t slotOf(
            const OwnedChunk& chunk) const noexcept
        {
            return static_cast<std::uint32_t>(&chunk - chunks.data());
        }

        [[nodiscard]] OwnedChunk* allocate() noexcept
        {
            const auto free = std::find_if(
                chunks.begin(), chunks.end(),
                [](const OwnedChunk& chunk) noexcept
                {
                    return chunk.state == EOwnedState::FREE &&
                        chunk.generation != 0u;
                });
            if (free != chunks.end())
                return &*free;
            if (chunks.size() >= config.maximum_tracked_chunks)
                return nullptr;
            chunks.emplace_back();
            return &chunks.back();
        }

        [[nodiscard]] bool enqueue(
            TilemapChunkIntentCommand command) noexcept
        {
            if (commands.push(command))
            {
                ++metrics.commands_enqueued;
                return true;
            }
            ++metrics.command_rejections;
            return false;
        }

        void enqueueReconcile(entt::entity entity) noexcept
        {
            if (!closing)
            {
                static_cast<void>(enqueue(TilemapChunkIntentCommand{
                    ETilemapChunkIntentAction::RECONCILE,
                    entity}));
            }
        }

        void enqueueRetire(
            std::uint32_t slot,
            std::uint32_t generation) noexcept
        {
            auto* chunk = find(slot, generation);
            if (!chunk || chunk->retire_requested)
                return;
            if (enqueue(TilemapChunkIntentCommand{
                    ETilemapChunkIntentAction::RETIRE,
                    entt::null,
                    slot,
                    generation}))
            {
                chunk->retire_requested = true;
            }
        }

        void enqueueObservedRetire(
            std::uint32_t slot,
            std::uint32_t generation) noexcept
        {
            static_cast<void>(enqueue(TilemapChunkIntentCommand{
                ETilemapChunkIntentAction::RETIRE,
                entt::null,
                slot,
                generation}));
        }

        void requestRetire(OwnedChunk& chunk) noexcept
        {
            enqueueRetire(slotOf(chunk), chunk.generation);
        }

        [[nodiscard]] bool desiredActive(
            lux::ecs::TileChunkCoord coordinate) const noexcept
        {
            return !activity || activity->isActive(coordinate);
        }

        void chunkChanged(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            enqueueReconcile(entity);
        }

        void bindingDestroyed(
            lux::ecs::RegistryBase& registry,
            entt::entity entity) noexcept
        {
            const auto* binding =
                registry.try_get<TilemapChunkBindingComponent>(entity);
            if (binding)
            {
                // An EnTT observer records only a value intent. The owned
                // state is changed later when this command reaches the
                // Schedule barrier.
                enqueueObservedRetire(
                    binding->slot, binding->generation);
            }
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
                lux::ecs::TileChunk2DComponent>()
                .connect<&Impl::chunkChanged>(*this);
            updated = registry.on_update<
                lux::ecs::TileChunk2DComponent>()
                .connect<&Impl::chunkChanged>(*this);
            destroyed = registry.on_destroy<
                lux::ecs::TileChunk2DComponent>()
                .connect<&Impl::chunkChanged>(*this);
            binding_destroyed = registry.on_destroy<
                TilemapChunkBindingComponent>()
                .connect<&Impl::bindingDestroyed>(*this);
            for (const auto entity :
                 registry.view<const lux::ecs::TileChunk2DComponent>())
            {
                enqueueReconcile(entity);
            }
        }

        void detach() noexcept
        {
            constructed.release();
            updated.release();
            destroyed.release();
            binding_destroyed.release();
            attached = nullptr;
            commands = {};
        }

        void setStatus(
            lux::ecs::Registry& registry,
            entt::entity entity,
            ETilemapChunkDomainState state,
            ETilemapChunkDomainError error,
            std::uint64_t generation)
        {
            if (!registry.valid(entity) ||
                !registry.all_of<lux::ecs::TileChunk2DComponent>(entity))
            {
                return;
            }
            registry.emplace_or_replace<TilemapChunkDomainStateComponent>(
                entity,
                TilemapChunkDomainStateComponent{
                    state, error, generation});
        }

        void hide(OwnedChunk& chunk) noexcept
        {
            if (!chunk.adopted || !chunk.active ||
                !runtime->isAlive(chunk.tilemap))
            {
                return;
            }
            static_cast<void>(runtime->setChunkActive(
                chunk.tilemap, chunk.coordinate, false));
            chunk.active = false;
            ++metrics.hidden_chunks;
        }

        void launch(OwnedChunk& chunk) noexcept
        {
            if (chunk.state != EOwnedState::WAITING_ADMISSION ||
                chunk.retire_requested)
            {
                return;
            }
            const auto slot = slotOf(chunk);
            const auto generation = chunk.generation;
            if (!preparation)
            {
                chunk.prepare_error =
                    ETilemapChunkDomainError::CONTENT_UNAVAILABLE;
                chunk.state = EOwnedState::READY_TO_PUBLISH;
                return;
            }
            auto pipeline = lux::exec::execute(
                    preparation.operation(),
                    PrepareTilemapChunk{
                        chunk.content.bytes(),
                        chunk.coordinate,
                        chunk.content_reference.id.digest,
                        generation},
                    lux::async::SubmitOptions{
                        .accounted_bytes = chunk.content.bytes().size() +
                            lux::ecs::TilemapRuntime::kChunkTileCount *
                                sizeof(std::uint16_t)})
                | stdexec::continues_on(
                      lux::exec::mainThreadScheduler(*async_runtime))
                | stdexec::then(
                      [weak = std::weak_ptr{completion}, slot, generation](
                          lux::async::OperationOutcome<PrepareTilemapChunk>
                              outcome) mutable noexcept
                      {
                          const auto locked = weak.lock();
                          if (!locked)
                              return;
                          if (auto* owner = locked->owner.load(
                                  std::memory_order_acquire))
                          {
                              owner->acceptPreparation(
                                  slot, generation, std::move(outcome));
                          }
                      })
                | stdexec::upon_stopped(
                      [weak = std::weak_ptr{completion}, slot, generation]()
                          noexcept
                      {
                          const auto locked = weak.lock();
                          if (!locked)
                              return;
                          if (auto* owner = locked->owner.load(
                                  std::memory_order_acquire))
                          {
                              owner->acceptPreparationStopped(
                                  slot, generation);
                          }
                      });
            chunk.state = EOwnedState::WAITING_BACKGROUND;
            ++metrics.preparation_attempts;
            if (!lux::exec::spawn(*scene_scope, std::move(pipeline)))
            {
                chunk.state = EOwnedState::WAITING_ADMISSION;
                ++metrics.queue_backpressure;
            }
        }

        void release(OwnedChunk& chunk) noexcept
        {
            chunk.content = {};
            chunk.prepared.reset();
            chunk.entity = entt::null;
            chunk.tilemap = {};
            chunk.coordinate = {};
            chunk.tilemap_reference = {};
            chunk.content_reference = {};
            chunk.prepare_error = ETilemapChunkDomainError::NONE;
            chunk.active = false;
            chunk.adopted = false;
            chunk.retire_requested = false;
            chunk.state = EOwnedState::FREE;
            ++chunk.generation;
            if (chunk.generation == 0u)
                chunk.generation = 0u;
            ++metrics.retired_chunks;
        }

        void retireBounded() noexcept
        {
            metrics.retirement_granules_last_update = 0u;
            for (std::uint32_t retired = 0u;
                 retired < config.maximum_retirements_per_update;
                 ++retired)
            {
                const auto found = std::find_if(
                    chunks.begin(), chunks.end(),
                    [](const OwnedChunk& chunk) noexcept
                    {
                        return chunk.state == EOwnedState::RETIRING;
                    });
                if (found == chunks.end())
                    break;
                if (found->adopted && runtime->isAlive(found->tilemap) &&
                    runtime->chunkResident(
                        found->tilemap, found->coordinate))
                {
                    if (!runtime->discardChunk(
                            found->tilemap, found->coordinate))
                    {
                        break;
                    }
                }
                release(*found);
                ++metrics.retirement_granules_last_update;
            }
            metrics.maximum_retirement_granules_per_update = std::max(
                metrics.maximum_retirement_granules_per_update,
                metrics.retirement_granules_last_update);
        }

        void recount(lux::ecs::Registry& registry) noexcept
        {
            metrics.waiting_chunks = 0u;
            metrics.staging_chunks = 0u;
            metrics.ready_chunks = 0u;
            metrics.failed_chunks = 0u;
            registry.view<const TilemapChunkDomainStateComponent>().each(
                [this](const TilemapChunkDomainStateComponent& state)
                {
                    switch (state.state)
                    {
                    case ETilemapChunkDomainState::WAITING_TILEMAP:
                        ++metrics.waiting_chunks;
                        break;
                    case ETilemapChunkDomainState::STAGING:
                        ++metrics.staging_chunks;
                        break;
                    case ETilemapChunkDomainState::READY:
                        ++metrics.ready_chunks;
                        break;
                    case ETilemapChunkDomainState::FAILED:
                        ++metrics.failed_chunks;
                        break;
                    }
                });
            metrics.tracked_chunks = 0u;
            metrics.background_chunks = 0u;
            metrics.retiring_chunks = 0u;
            metrics.owned_blob_leases = 0u;
            for (const auto& chunk : chunks)
            {
                if (chunk.state == EOwnedState::FREE)
                    continue;
                ++metrics.tracked_chunks;
                if (chunk.content)
                    ++metrics.owned_blob_leases;
                if (chunk.state == EOwnedState::WAITING_BACKGROUND)
                    ++metrics.background_chunks;
                if (chunk.retire_requested ||
                    chunk.state == EOwnedState::RETIRING)
                {
                    ++metrics.retiring_chunks;
                }
            }
            metrics.closed = closing && close_fence_applied &&
                metrics.tracked_chunks == 0u &&
                metrics.commands_enqueued == metrics.commands_applied &&
                registry.storage<TilemapChunkBindingComponent>().empty() &&
                registry.storage<TilemapChunkDomainStateComponent>().empty();
        }

        lux::exec::AsyncRuntime* async_runtime{nullptr};
        lux::exec::AsyncScope* scene_scope{nullptr};
        TilemapPrepareClient preparation;
        lux::ecs::TilemapRuntime* runtime{nullptr};
        lux::ecs::TilemapSystem* tilemaps{nullptr};
        lux::runtime::entity_scene::ContentBlobClient content;
        const TilemapChunkActivity2D* activity{nullptr};
        TilemapChunkSystemConfig config;
        std::shared_ptr<CompletionControl> completion;
        lux::ecs::Registry* attached{nullptr};
        lux::ecs::EcsCommandWriter commands;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        entt::scoped_connection binding_destroyed;
        std::vector<OwnedChunk> chunks;
        TilemapChunkSystemSnapshot metrics;
        lux::ecs::SystemCloseProgressSink close_progress;
        bool closing{false};
        bool close_fence_queued{false};
        bool close_fence_applied{false};
    };

    TilemapChunkSystem::TilemapChunkSystem(
        lux::exec::AsyncRuntime& async_runtime,
        lux::exec::AsyncScope& scene_scope,
        TilemapPrepareClient preparation,
        lux::ecs::TilemapRuntime& runtime,
        lux::ecs::TilemapSystem& tilemaps,
        lux::runtime::entity_scene::ContentBlobClient content,
        const TilemapChunkActivity2D* activity,
        TilemapChunkSystemConfig config)
        : impl_(std::make_unique<Impl>(
              *this,
              async_runtime,
              scene_scope,
              std::move(preparation),
              runtime,
              tilemaps,
              std::move(content),
              activity,
              config))
    {}

    TilemapChunkSystem::~TilemapChunkSystem()
    {
        impl_->completion->owner.store(nullptr, std::memory_order_release);
        if (impl_->attached)
            impl_->detach();
    }

    void TilemapChunkSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void TilemapChunkSystem::onRemoved(
        const lux::ecs::SystemRemovalContext& removal)
    {
        if (!impl_->closing || !closeComplete())
            std::abort();
        if (impl_->attached == &removal.registry())
            impl_->detach();
    }

    void TilemapChunkSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        if (impl_->attached != &context.registry())
            std::abort();
        auto& registry = context.registry();
        impl_->retireBounded();

        for (auto& chunk : impl_->chunks)
        {
            if (chunk.state == EOwnedState::FREE)
                continue;
            if (impl_->closing && !chunk.retire_requested)
                impl_->requestRetire(chunk);
            if (!impl_->closing && !chunk.retire_requested)
            {
                if (!registry.valid(chunk.entity) ||
                    !registry.all_of<lux::ecs::TileChunk2DComponent>(
                        chunk.entity))
                {
                    impl_->requestRetire(chunk);
                    continue;
                }
                const auto current = impl_->tilemaps->resolveTilemap(
                    chunk.tilemap_reference);
                if (!current.isValid() || current != chunk.tilemap)
                {
                    impl_->requestRetire(chunk);
                    continue;
                }
            }
            if (!impl_->closing &&
                chunk.state == EOwnedState::WAITING_ADMISSION)
            {
                impl_->launch(chunk);
            }
            if (!impl_->closing &&
                chunk.state == EOwnedState::READY_TO_PUBLISH)
            {
                static_cast<void>(impl_->enqueue(
                    TilemapChunkIntentCommand{
                        ETilemapChunkIntentAction::PUBLISH,
                        chunk.entity,
                        impl_->slotOf(chunk),
                        chunk.generation}));
            }
            if (chunk.state != EOwnedState::PUBLISHED)
                continue;
            const bool active = impl_->desiredActive(chunk.coordinate);
            if (active != chunk.active &&
                impl_->runtime->setChunkActive(
                    chunk.tilemap, chunk.coordinate, active))
            {
                chunk.active = active;
                if (!active)
                    ++impl_->metrics.hidden_chunks;
            }
        }

        if (!impl_->closing)
        {
            for (const auto entity :
                 registry.view<const lux::ecs::TileChunk2DComponent>())
            {
                auto* chunk = impl_->find(entity);
                if (!chunk)
                {
                    const auto* status = registry.try_get<
                        TilemapChunkDomainStateComponent>(entity);
                    if (!status || status->state !=
                            ETilemapChunkDomainState::FAILED)
                    {
                        impl_->enqueueReconcile(entity);
                    }
                    continue;
                }
                const auto& fact = registry.get<
                    lux::ecs::TileChunk2DComponent>(entity);
                if (!sameFact(*chunk, fact))
                    impl_->enqueueReconcile(entity);
            }
        }

        if (impl_->closing && !impl_->close_fence_queued &&
            !impl_->close_fence_applied &&
            impl_->enqueue(TilemapChunkIntentCommand{
                ETilemapChunkIntentAction::CLOSE_FENCE}))
        {
            impl_->close_fence_queued = true;
        }
        impl_->recount(registry);
    }

    void TilemapChunkSystem::applyReconcile(
        lux::ecs::Registry& registry,
        entt::entity entity) noexcept
    {
        if (impl_->attached != &registry)
            std::abort();
        const auto* fact =
            registry.try_get<lux::ecs::TileChunk2DComponent>(entity);
        if (auto* existing = impl_->find(entity))
        {
            if (!fact || impl_->closing || !sameFact(*existing, *fact) ||
                existing->state == EOwnedState::FAILED)
            {
                impl_->requestRetire(*existing);
            }
            return;
        }
        if (impl_->closing || !fact)
            return;

        const auto tilemap = impl_->tilemaps->resolveTilemap(fact->tilemap);
        if (!tilemap.isValid())
        {
            impl_->setStatus(
                registry,
                entity,
                ETilemapChunkDomainState::WAITING_TILEMAP,
                ETilemapChunkDomainError::NONE,
                0u);
            return;
        }
        if (!fact->content.valid() ||
            fact->content.type.name() !=
                lux::tilemap::kTilemapChunkContentTypeName ||
            fact->content.schema_version !=
                lux::tilemap::kTilemapChunkSchemaVersion)
        {
            impl_->setStatus(
                registry,
                entity,
                ETilemapChunkDomainState::FAILED,
                ETilemapChunkDomainError::CONTENT_INVALID,
                0u);
            return;
        }
        auto content = impl_->content.resolve(fact->content);
        if (!content)
        {
            impl_->setStatus(
                registry,
                entity,
                ETilemapChunkDomainState::FAILED,
                ETilemapChunkDomainError::CONTENT_UNAVAILABLE,
                0u);
            return;
        }
        auto* owned = impl_->allocate();
        if (!owned)
        {
            ++impl_->metrics.capacity_rejections;
            impl_->setStatus(
                registry,
                entity,
                ETilemapChunkDomainState::FAILED,
                ETilemapChunkDomainError::CAPACITY_EXHAUSTED,
                0u);
            return;
        }
        owned->state = EOwnedState::WAITING_ADMISSION;
        owned->entity = entity;
        owned->tilemap = tilemap;
        owned->coordinate = fact->coordinate;
        owned->tilemap_reference = fact->tilemap;
        owned->content_reference = fact->content;
        owned->content = std::move(*content);
        owned->prepared.reset();
        owned->prepare_error = ETilemapChunkDomainError::NONE;
        owned->active = false;
        owned->adopted = false;
        owned->retire_requested = false;
        impl_->setStatus(
            registry,
            entity,
            ETilemapChunkDomainState::STAGING,
            ETilemapChunkDomainError::NONE,
            owned->generation);
    }

    void TilemapChunkSystem::applyPublish(
        lux::ecs::Registry& registry,
        entt::entity entity,
        std::uint32_t slot,
        std::uint32_t generation) noexcept
    {
        auto* owned = impl_->find(slot, generation);
        const auto* fact =
            registry.try_get<lux::ecs::TileChunk2DComponent>(entity);
        if (!owned || owned->state != EOwnedState::READY_TO_PUBLISH)
            return;
        if (impl_->closing || owned->retire_requested || !fact ||
            owned->entity != entity || !sameFact(*owned, *fact))
        {
            impl_->requestRetire(*owned);
            return;
        }
        if (owned->prepare_error != ETilemapChunkDomainError::NONE ||
            !owned->prepared)
        {
            const auto error = owned->prepare_error ==
                    ETilemapChunkDomainError::NONE
                ? ETilemapChunkDomainError::CONTENT_INVALID
                : owned->prepare_error;
            owned->prepared.reset();
            owned->content = {};
            owned->state = EOwnedState::FAILED;
            impl_->setStatus(
                registry,
                entity,
                ETilemapChunkDomainState::FAILED,
                error,
                generation);
            return;
        }

        const bool active = impl_->desiredActive(owned->coordinate);
        owned->prepared->active = active;
        if (!impl_->runtime->loadChunk(
                owned->tilemap, std::move(*owned->prepared)))
        {
            owned->prepared.reset();
            owned->content = {};
            owned->state = EOwnedState::FAILED;
            impl_->setStatus(
                registry,
                entity,
                ETilemapChunkDomainState::FAILED,
                ETilemapChunkDomainError::RUNTIME_REJECTED,
                generation);
            return;
        }
        owned->prepared.reset();
        owned->adopted = true;
        owned->active = active;
        owned->state = EOwnedState::PUBLISHED;
        registry.emplace_or_replace<TilemapChunkBindingComponent>(
            entity,
            TilemapChunkBindingComponent{slot, generation});
        impl_->setStatus(
            registry,
            entity,
            ETilemapChunkDomainState::READY,
            ETilemapChunkDomainError::NONE,
            generation);
        ++impl_->metrics.published_chunks;
    }

    void TilemapChunkSystem::applyRetire(
        lux::ecs::Registry& registry,
        std::uint32_t slot,
        std::uint32_t generation) noexcept
    {
        auto* owned = impl_->find(slot, generation);
        if (!owned)
            return;
        owned->retire_requested = true;
        impl_->hide(*owned);
        if (registry.valid(owned->entity))
        {
            const auto* binding = registry.try_get<
                TilemapChunkBindingComponent>(owned->entity);
            if (binding && binding->slot == slot &&
                binding->generation == generation)
            {
                registry.remove<TilemapChunkBindingComponent>(
                    owned->entity);
            }
            registry.remove<TilemapChunkDomainStateComponent>(
                owned->entity);
        }
        if (owned->state != EOwnedState::WAITING_BACKGROUND)
            owned->state = EOwnedState::RETIRING;
    }

    void TilemapChunkSystem::applyCloseFence() noexcept
    {
        impl_->close_fence_queued = false;
        impl_->close_fence_applied = true;
    }

    void TilemapChunkSystem::acceptPreparation(
        std::uint32_t slot,
        std::uint32_t generation,
        lux::async::OperationOutcome<PrepareTilemapChunk> outcome) noexcept
    {
        auto* owned = impl_->find(slot, generation);
        if (!owned || owned->state != EOwnedState::WAITING_BACKGROUND)
        {
            ++impl_->metrics.stale_completions;
            return;
        }
        if (owned->retire_requested || impl_->closing)
        {
            owned->state = EOwnedState::RETIRING;
            if (impl_->close_progress)
                impl_->close_progress.notify();
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
                    owned->state = EOwnedState::WAITING_ADMISSION;
                    ++impl_->metrics.queue_backpressure;
                    return;
                }
            }
            owned->prepare_error =
                ETilemapChunkDomainError::CONTENT_INVALID;
        }
        else if (outcome->request_generation != generation ||
                 outcome->load.coordinate != owned->coordinate)
        {
            ++impl_->metrics.stale_completions;
            owned->prepare_error =
                ETilemapChunkDomainError::CONTENT_INVALID;
        }
        else
        {
            owned->prepared.emplace(std::move(outcome->load));
        }
        owned->state = EOwnedState::READY_TO_PUBLISH;
        static_cast<void>(impl_->enqueue(TilemapChunkIntentCommand{
            ETilemapChunkIntentAction::PUBLISH,
            owned->entity,
            slot,
            generation}));
        if (impl_->close_progress)
            impl_->close_progress.notify();
    }

    void TilemapChunkSystem::acceptPreparationStopped(
        std::uint32_t slot,
        std::uint32_t generation) noexcept
    {
        auto* owned = impl_->find(slot, generation);
        if (!owned || owned->state != EOwnedState::WAITING_BACKGROUND)
        {
            ++impl_->metrics.stale_completions;
            return;
        }
        if (owned->retire_requested || impl_->closing)
            owned->state = EOwnedState::RETIRING;
        else
        {
            owned->prepare_error =
                ETilemapChunkDomainError::CONTENT_UNAVAILABLE;
            owned->state = EOwnedState::READY_TO_PUBLISH;
            static_cast<void>(impl_->enqueue(TilemapChunkIntentCommand{
                ETilemapChunkIntentAction::PUBLISH,
                owned->entity,
                slot,
                generation}));
        }
        if (impl_->close_progress)
            impl_->close_progress.notify();
    }

    void TilemapChunkSystem::requestClose() noexcept
    {
        requestClose({});
    }

    void TilemapChunkSystem::requestClose(
        lux::ecs::SystemCloseProgressSink progress) noexcept
    {
        if (progress)
            impl_->close_progress = progress;
        if (impl_->closing)
            return;
        impl_->closing = true;
        impl_->metrics.closing = true;
        for (auto& chunk : impl_->chunks)
            if (chunk.state != EOwnedState::FREE)
                impl_->requestRetire(chunk);
    }

    bool TilemapChunkSystem::closeComplete() const noexcept
    {
        if (!impl_->closing || !impl_->close_fence_applied ||
            !impl_->attached)
        {
            return false;
        }
        return std::all_of(
                   impl_->chunks.begin(), impl_->chunks.end(),
                   [](const OwnedChunk& chunk) noexcept
                   {
                       return chunk.state == EOwnedState::FREE;
                   }) &&
            impl_->metrics.commands_enqueued ==
                impl_->metrics.commands_applied &&
            impl_->attached->storage<TilemapChunkBindingComponent>().empty() &&
            impl_->attached->storage<
                TilemapChunkDomainStateComponent>().empty();
    }

    bool TilemapChunkSystem::closeNeedsOwnerTick() const noexcept
    {
        if (!impl_->closing || closeComplete())
            return false;
        if (!impl_->close_fence_applied)
            return true;
        if (impl_->metrics.commands_enqueued !=
            impl_->metrics.commands_applied)
        {
            return true;
        }
        return std::any_of(
            impl_->chunks.begin(), impl_->chunks.end(),
            [](const OwnedChunk& chunk) noexcept
            {
                return chunk.state != EOwnedState::FREE &&
                    chunk.state != EOwnedState::WAITING_BACKGROUND;
            });
    }

    TilemapChunkSystemSnapshot TilemapChunkSystem::snapshot() const noexcept
    {
        auto result = impl_->metrics;
        result.closed = closeComplete();
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    TilemapChunkSystem::prerequisites() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<lux::ecs::TilemapSystem>()};
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    TilemapChunkSystem::runsAfter() const noexcept
    {
        return prerequisites();
    }

    void TilemapChunkIntentCommand::prepareRegistryPublication(
        lux::ecs::Registry& registry) const noexcept
    {
        lux::ecs::reserveEcsCommandStorage(
            registry.storage<TilemapChunkDomainStateComponent>(), 1u);
        lux::ecs::reserveEcsCommandStorage(
            registry.storage<TilemapChunkBindingComponent>(), 1u);
    }

    void TilemapChunkIntentCommand::apply(
        lux::ecs::Registry& registry,
        TilemapChunkSystem& system) const noexcept
    {
        switch (action)
        {
        case ETilemapChunkIntentAction::RECONCILE:
            system.applyReconcile(registry, entity);
            break;
        case ETilemapChunkIntentAction::PUBLISH:
            system.applyPublish(registry, entity, slot, generation);
            break;
        case ETilemapChunkIntentAction::RETIRE:
            system.applyRetire(registry, slot, generation);
            break;
        case ETilemapChunkIntentAction::CLOSE_FENCE:
            system.applyCloseFence();
            break;
        }
        ++system.impl_->metrics.commands_applied;
    }
} // namespace lux::runtime::spatial2d
