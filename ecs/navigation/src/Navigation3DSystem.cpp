#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>

#include <lux/engine/ecs/navigation/components/NavigationRegion3DComponent.hpp>

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        using BackendError =
            lux::navigation::detour3d::ENavigationRegion3DError;

        [[nodiscard]] ENavigationRegion3DFailureCode
        mapFailure(BackendError value) noexcept
        {
            switch (value)
            {
            case BackendError::INVALID_REQUEST:
                return ENavigationRegion3DFailureCode::INVALID_REFERENCE;
            case BackendError::INVALID_CONTENT:
                return ENavigationRegion3DFailureCode::INVALID_CONTENT;
            case BackendError::UNSUPPORTED_AGENT:
                return ENavigationRegion3DFailureCode::UNSUPPORTED_AGENT;
            case BackendError::CAPACITY_EXHAUSTED:
                return ENavigationRegion3DFailureCode::CAPACITY_EXHAUSTED;
            case BackendError::BUILD_FAILED:
                return ENavigationRegion3DFailureCode::BUILD_FAILED;
            case BackendError::REGION_CONFLICT:
                return ENavigationRegion3DFailureCode::REGION_CONFLICT;
            case BackendError::STALE_GENERATION:
                return ENavigationRegion3DFailureCode::STALE_GENERATION;
            }
            return ENavigationRegion3DFailureCode::BUILD_FAILED;
        }

        [[nodiscard]] std::uint32_t entityKey(entt::entity entity) noexcept
        {
            return static_cast<std::uint32_t>(entt::to_integral(entity));
        }

        [[nodiscard]] bool supportedContent(
            const lux::entity_scene::ContentBlobRef& content) noexcept
        {
            return content.valid() &&
                   content.type.name() ==
                       lux::navigation::detour3d::
                           kNavigationRegion3DContentTypeName &&
                   content.schema_version ==
                       lux::navigation::detour3d::
                           kNavigationRegion3DSchemaVersion;
        }
    } // namespace

    struct Navigation3DSystem::Impl final
    {
        using Lease = lux::navigation::detour3d::NavigationRegion3DLease;

        struct Entry final
        {
            entt::entity entity{entt::null};
            lux::entity_scene::ContentBlobRef content;
            std::uint64_t generation{0u};
            ENavigationRegion3DState state{
                ENavigationRegion3DState::WAITING_BACKGROUND};
            ENavigationRegion3DFailureCode failure{
                ENavigationRegion3DFailureCode::NONE};
            bool request_pending{false};
            bool completion_pending{false};
            bool publish_pending{false};
            bool failure_pending{false};
            bool close_command_pending{false};
            std::unique_ptr<Lease> lease;
        };

        struct Completion final
        {
            entt::entity entity{entt::null};
            std::uint64_t generation{0u};
            std::optional<lux::navigation::detour3d::PreparedNavigationRegion3D>
                prepared;
            std::optional<lux::navigation::detour3d::NavigationRegion3DFailure>
                failure;
        };

        explicit Impl(
            std::shared_ptr<lux::navigation::detour3d::Navigation3DBackend>
                backend_value,
            Navigation3DSystemConfig config_value)
            : backend(std::move(backend_value)),
              config(config_value),
              owner_thread(std::this_thread::get_id())
        {
            if (!backend || config.maximum_pending_requests == 0u ||
                config.maximum_pending_completions == 0u ||
                config.maximum_staging_granules_per_tick == 0u ||
                config.maximum_retirement_granules_per_tick == 0u ||
                config.maximum_close_hides_per_tick == 0u)
            {
                std::abort();
            }
            requests.reserve(config.maximum_pending_requests);
            completions.reserve(config.maximum_pending_completions);
            discarded_prepared.reserve(
                config.maximum_pending_completions);
        }

        void enqueue(entt::entity entity) noexcept
        {
            if (!commands.push(Navigation3DIntentCommand{
                    ENavigation3DIntentAction::RECONCILE,
                    entity,
                    0u,
                    ENavigationRegion3DFailureCode::NONE}))
            {
                ++snapshot.stale_completions;
            }
        }

        void onConstructed(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity);
        }

        void onUpdated(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity);
        }

        void onDestroyed(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity);
        }

        void attach(
            lux::meta::EntityRegistryBase& registry,
            EcsCommandWriter writer)
        {
            if (attached)
                std::abort();
            attached = &registry;
            commands = writer;
            constructed = registry.on_construct<NavigationRegion3DComponent>()
                              .connect<&Impl::onConstructed>(*this);
            updated = registry.on_update<NavigationRegion3DComponent>()
                          .connect<&Impl::onUpdated>(*this);
            destroyed = registry.on_destroy<NavigationRegion3DComponent>()
                            .connect<&Impl::onDestroyed>(*this);
            for (const auto entity :
                 registry.view<const NavigationRegion3DComponent>())
            {
                enqueue(entity);
            }
        }

        void detach(lux::meta::EntityRegistry& registry) noexcept
        {
            if (!closeComplete())
                std::abort();
            constructed.release();
            updated.release();
            destroyed.release();
            if (!registry.view<const NavigationRegion3DStatusComponent>()
                     .empty())
                std::abort();
            attached = nullptr;
            commands = {};
        }

        [[nodiscard]] std::uint64_t nextGeneration() noexcept
        {
            ++next_generation;
            if (next_generation == 0u)
                ++next_generation;
            return next_generation;
        }

        void queueRetirement(std::unique_ptr<Lease> lease) noexcept
        {
            if (!lease)
                return;
            if (lease->state() !=
                    lux::navigation::detour3d::
                        ENavigationRegion3DLeaseState::RETIRING)
            {
                const auto begun = lease->beginRetirement();
                if (!begun)
                    std::abort();
            }
            retirements.push_back(std::move(lease));
        }

        void queueRetirement(Entry& entry) noexcept
        {
            entry.state = ENavigationRegion3DState::RETIRING;
            queueRetirement(std::move(entry.lease));
            entry.publish_pending = false;
            entry.failure_pending = false;
            entry.completion_pending = false;
        }

        [[nodiscard]] bool tryQueuePreparedRetirement(
            lux::navigation::detour3d::PreparedNavigationRegion3D&&
                prepared) noexcept
        {
            if (!prepared.valid())
                return true;
            // The stale/cancel queue shares the completion admission bound.
            // A main-thread producer retains the still-valid value when this
            // window is full and retries after one retirement granule frees a
            // slot. This keeps both allocations and owner bytes bounded.
            if (discarded_prepared.size() >=
                config.maximum_pending_completions)
            {
                return false;
            }
            const auto bytes = prepared.ownedBytes();
            if (discarded_prepared_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - bytes)
            {
                std::abort();
            }
            discarded_prepared_bytes += bytes;
            discarded_prepared.push_back(std::move(prepared));
            return true;
        }

        [[nodiscard]] bool closeComplete() const noexcept
        {
            const auto backend_state = backend->snapshot();
            return closing && entries.empty() && retirements.empty() &&
                   discarded_prepared.empty() &&
                   discarded_prepared_bytes == 0u && completions.empty() &&
                   completion_prepared_bytes == 0u &&
                   request_read_offset == requests.size() &&
                   backend_state.staged_regions == 0u &&
                   backend_state.active_regions == 0u &&
                   backend_state.retiring_regions == 0u &&
                   backend_state.owned_bytes == 0u;
        }

        std::shared_ptr<lux::navigation::detour3d::Navigation3DBackend> backend;
        Navigation3DSystemConfig config;
        const std::thread::id owner_thread;
        lux::meta::EntityRegistryBase* attached{nullptr};
        EcsCommandWriter commands;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        std::map<std::uint32_t, Entry> entries;
        std::vector<std::unique_ptr<Lease>> retirements;
        std::vector<lux::navigation::detour3d::
                        PreparedNavigationRegion3D>
            discarded_prepared;
        std::uint64_t discarded_prepared_bytes{0u};
        std::vector<NavigationRegion3DPrepareRequest> requests;
        std::size_t request_read_offset{0u};
        std::vector<Completion> completions;
        std::uint64_t completion_prepared_bytes{0u};
        std::uint64_t next_generation{0u};
        std::uint32_t staging_cursor{0u};
        std::uint32_t close_cursor{0u};
        std::size_t retirement_cursor{0u};
        std::size_t prepared_retirement_cursor{0u};
        bool retire_prepared_next{true};
        bool closing{false};
        Navigation3DSystemSnapshot snapshot;
    };

    Navigation3DSystem::Navigation3DSystem(
        std::shared_ptr<lux::navigation::detour3d::Navigation3DBackend> backend,
        Navigation3DSystemConfig config)
        : impl_(std::make_unique<Impl>(std::move(backend), config))
    {
    }

    Navigation3DSystem::~Navigation3DSystem()
    {
        // A published system must pass through the shared Schedule close
        // gate. Otherwise prepared batches in private queues could fall back
        // to vector destruction and recreate an unbudgeted close tail.
        if (impl_ && impl_->attached && !impl_->closeComplete())
            std::abort();
    }

    void Navigation3DSystem::requireOwnerThread() const noexcept
    {
        if (std::this_thread::get_id() != impl_->owner_thread)
            std::abort();
    }

    void Navigation3DSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void Navigation3DSystem::onRemoved(const SystemRemovalContext& removal)
    {
        impl_->detach(removal.registry());
    }

    void Navigation3DSystem::applyReconcile(
        lux::meta::EntityRegistry& registry,
        entt::entity entity) noexcept
    {
        if (impl_->attached != &registry)
            std::abort();
        const auto key = entityKey(entity);
        const auto* component =
            registry.valid(entity)
                ? registry.try_get<NavigationRegion3DComponent>(entity)
                : nullptr;
        auto found = impl_->entries.find(key);

        if (impl_->closing || !component)
        {
            if (found != impl_->entries.end())
            {
                impl_->queueRetirement(found->second);
                impl_->entries.erase(found);
            }
            if (registry.valid(entity))
                registry.remove<NavigationRegion3DStatusComponent>(entity);
            return;
        }

        if (found != impl_->entries.end() &&
            found->second.content == component->content)
        {
            if (found->second.state == ENavigationRegion3DState::FAILED &&
                !found->second.lease && supportedContent(component->content))
            {
                found->second.generation = impl_->nextGeneration();
                found->second.state =
                    ENavigationRegion3DState::WAITING_BACKGROUND;
                found->second.failure = ENavigationRegion3DFailureCode::NONE;
                found->second.request_pending = true;
                found->second.completion_pending = false;
                found->second.failure_pending = false;
                registry.emplace_or_replace<NavigationRegion3DStatusComponent>(
                    entity,
                    NavigationRegion3DStatusComponent{
                        found->second.state,
                        found->second.failure,
                        found->second.generation});
            }
            return;
        }
        if (found != impl_->entries.end())
        {
            impl_->queueRetirement(found->second);
            impl_->entries.erase(found);
        }

        Impl::Entry entry;
        entry.entity = entity;
        entry.content = component->content;
        entry.generation = impl_->nextGeneration();
        if (!supportedContent(component->content))
        {
            entry.state = ENavigationRegion3DState::FAILED;
            entry.failure = ENavigationRegion3DFailureCode::INVALID_REFERENCE;
            ++impl_->snapshot.failed_regions;
        }
        else
        {
            entry.request_pending = true;
        }
        registry.emplace_or_replace<NavigationRegion3DStatusComponent>(
            entity,
            NavigationRegion3DStatusComponent{
                entry.state, entry.failure, entry.generation});
        impl_->entries.emplace(key, std::move(entry));
    }

    void Navigation3DSystem::update(const SystemUpdateContext& context)
    {
        if (impl_->attached != &context.registry())
            std::abort();
        auto& registry = context.registry();

        if (impl_->closing)
        {
            std::uint32_t close_hides = 0u;
            if (!impl_->entries.empty())
            {
                auto current = impl_->entries.upper_bound(
                    impl_->close_cursor);
                std::size_t visited = 0u;
                while (visited < impl_->entries.size() &&
                       close_hides <
                           impl_->config.maximum_close_hides_per_tick)
                {
                    if (current == impl_->entries.end())
                        current = impl_->entries.begin();
                    auto selected = current++;
                    ++visited;
                    auto& entry = selected->second;
                    if (!entry.close_command_pending)
                        continue;
                    if (context.commands().push(
                            Navigation3DIntentCommand{
                                ENavigation3DIntentAction::RECONCILE,
                                entry.entity,
                                entry.generation,
                                ENavigationRegion3DFailureCode::NONE}))
                    {
                        impl_->close_cursor = selected->first;
                        entry.close_command_pending = false;
                        ++close_hides;
                        ++impl_->snapshot.close_hides;
                    }
                    else
                    {
                        ++impl_->snapshot.queue_backpressure;
                    }
                }
            }
            impl_->snapshot.maximum_close_hides_per_tick = std::max(
                impl_->snapshot.maximum_close_hides_per_tick,
                close_hides);
        }
        else
        {
            for (auto& [_, entry] : impl_->entries)
            {
                if (!entry.request_pending || !registry.valid(entry.entity))
                    continue;
                const auto* component =
                    registry.try_get<NavigationRegion3DComponent>(
                        entry.entity);
                if (!component || component->content != entry.content)
                    continue;
                if (impl_->requests.size() >=
                        impl_->config.maximum_pending_requests &&
                    impl_->request_read_offset != 0u)
                {
                    impl_->requests.erase(
                        impl_->requests.begin(),
                        impl_->requests.begin() +
                            static_cast<std::ptrdiff_t>(
                                impl_->request_read_offset));
                    impl_->request_read_offset = 0u;
                }
                if (impl_->requests.size() >=
                    impl_->config.maximum_pending_requests)
                {
                    ++impl_->snapshot.queue_backpressure;
                    continue;
                }
                impl_->requests.push_back(
                    {entry.entity, entry.generation, component->content});
                entry.request_pending = false;
                ++impl_->snapshot.requests_emitted;
            }
        }

        std::size_t consumed_completions = 0u;
        for (; consumed_completions < impl_->completions.size();
             ++consumed_completions)
        {
            auto& completion = impl_->completions[consumed_completions];
            const auto completion_bytes = completion.prepared
                ? completion.prepared->ownedBytes()
                : 0u;
            const auto releaseCompletionBytes = [&]() noexcept
            {
                if (completion_bytes > impl_->completion_prepared_bytes)
                    std::abort();
                impl_->completion_prepared_bytes -= completion_bytes;
            };
            const auto found =
                impl_->entries.find(entityKey(completion.entity));
            if (impl_->closing || found == impl_->entries.end() ||
                found->second.entity != completion.entity ||
                found->second.generation != completion.generation ||
                !found->second.completion_pending)
            {
                if (completion.prepared &&
                    !impl_->tryQueuePreparedRetirement(
                        std::move(*completion.prepared)))
                {
                    ++impl_->snapshot.queue_backpressure;
                    break;
                }
                releaseCompletionBytes();
                ++impl_->snapshot.stale_completions;
                continue;
            }
            auto& entry = found->second;
            if (completion.failure)
            {
                entry.completion_pending = false;
                entry.failure = mapFailure(completion.failure->code);
                entry.state = ENavigationRegion3DState::FAILED;
                entry.failure_pending = true;
                ++impl_->snapshot.failed_regions;
                continue;
            }
            if (!completion.prepared ||
                completion.prepared->requestGeneration() != entry.generation)
            {
                if (completion.prepared &&
                    !impl_->tryQueuePreparedRetirement(
                        std::move(*completion.prepared)))
                {
                    ++impl_->snapshot.queue_backpressure;
                    break;
                }
                releaseCompletionBytes();
                entry.completion_pending = false;
                entry.failure =
                    ENavigationRegion3DFailureCode::STALE_GENERATION;
                entry.state = ENavigationRegion3DState::FAILED;
                entry.failure_pending = true;
                ++impl_->snapshot.failed_regions;
                continue;
            }
            // Adoption can reject without consuming its rvalue (capacity,
            // portal conflict, stale backend). Keep one discard slot ready so
            // that every such failure still enters bounded retirement.
            if (impl_->discarded_prepared.size() >=
                impl_->config.maximum_pending_completions)
            {
                ++impl_->snapshot.queue_backpressure;
                break;
            }
            auto adopted =
                impl_->backend->adoptPrepared(std::move(*completion.prepared));
            releaseCompletionBytes();
            entry.completion_pending = false;
            if (!adopted)
            {
                entry.failure = mapFailure(adopted.error().code);
                entry.state = ENavigationRegion3DState::FAILED;
                entry.failure_pending = true;
                ++impl_->snapshot.failed_regions;
                if (!impl_->tryQueuePreparedRetirement(
                        std::move(*completion.prepared)))
                {
                    std::abort();
                }
                continue;
            }
            entry.lease = std::move(*adopted);
            entry.state = ENavigationRegion3DState::STAGING;
        }
        if (consumed_completions == impl_->completions.size())
        {
            impl_->completions.clear();
        }
        else if (consumed_completions != 0u)
        {
            impl_->completions.erase(
                impl_->completions.begin(),
                impl_->completions.begin() +
                    static_cast<std::ptrdiff_t>(consumed_completions));
        }

        std::uint32_t staging_work = 0u;
        if (!impl_->entries.empty())
        {
            auto current = impl_->entries.upper_bound(impl_->staging_cursor);
            std::size_t visited = 0u;
            while (visited < impl_->entries.size() &&
                   staging_work <
                       impl_->config.maximum_staging_granules_per_tick)
            {
                if (current == impl_->entries.end())
                    current = impl_->entries.begin();
                auto selected = current++;
                ++visited;
                auto& entry = selected->second;
                if (entry.state != ENavigationRegion3DState::STAGING ||
                    !entry.lease)
                {
                    continue;
                }
                impl_->staging_cursor = selected->first;
                auto step = entry.lease->advancePreparationOne();
                if (!step)
                {
                    entry.failure = mapFailure(step.error().code);
                    entry.state = ENavigationRegion3DState::FAILED;
                    entry.failure_pending = true;
                    ++impl_->snapshot.failed_regions;
                    impl_->queueRetirement(std::move(entry.lease));
                    continue;
                }
                staging_work += step->work_items;
                impl_->snapshot.staging_work_items += step->work_items;
                impl_->snapshot.staging_bytes += step->bytes;
                if (step->complete)
                {
                    entry.state = ENavigationRegion3DState::READY;
                    entry.publish_pending = true;
                }
            }
        }

        for (auto& [_, entry] : impl_->entries)
        {
            if (!entry.publish_pending)
                continue;
            if (context.commands().push(Navigation3DIntentCommand{
                    ENavigation3DIntentAction::PUBLISH,
                    entry.entity,
                    entry.generation,
                    ENavigationRegion3DFailureCode::NONE}))
            {
                entry.publish_pending = false;
            }
            else
            {
                ++impl_->snapshot.queue_backpressure;
            }
        }

        for (auto& [_, entry] : impl_->entries)
        {
            if (!entry.failure_pending)
                continue;
            if (context.commands().push(Navigation3DIntentCommand{
                    ENavigation3DIntentAction::FAIL,
                    entry.entity,
                    entry.generation,
                    entry.failure}))
            {
                entry.failure_pending = false;
            }
            else
            {
                ++impl_->snapshot.queue_backpressure;
            }
        }

        std::uint32_t retirement_work = 0u;
        const auto retirePreparedOne = [&]() noexcept
        {
            if (impl_->discarded_prepared.empty())
                return false;
            if (impl_->prepared_retirement_cursor >=
                impl_->discarded_prepared.size())
            {
                impl_->prepared_retirement_cursor = 0u;
            }
            auto& prepared = impl_->discarded_prepared[
                impl_->prepared_retirement_cursor];
            auto step = prepared.advanceRetirementOne();
            if (!step || step->work_items != 1u ||
                step->bytes > impl_->discarded_prepared_bytes)
            {
                std::abort();
            }
            impl_->discarded_prepared_bytes -= step->bytes;
            retirement_work += step->work_items;
            impl_->snapshot.retirement_work_items += step->work_items;
            impl_->snapshot.retired_bytes += step->bytes;
            if (step->complete)
            {
                impl_->discarded_prepared.erase(
                    impl_->discarded_prepared.begin() +
                    static_cast<std::ptrdiff_t>(
                        impl_->prepared_retirement_cursor));
            }
            else if (!impl_->discarded_prepared.empty())
            {
                impl_->prepared_retirement_cursor =
                    (impl_->prepared_retirement_cursor + 1u) %
                    impl_->discarded_prepared.size();
            }
            impl_->retire_prepared_next = false;
            return true;
        };
        if (retirement_work <
                impl_->config.maximum_retirement_granules_per_tick &&
            !impl_->discarded_prepared.empty() &&
            (impl_->retire_prepared_next ||
             (impl_->retirements.empty() &&
              impl_->backend->snapshot().retiring_regions == 0u)))
        {
            (void)retirePreparedOne();
        }

        std::size_t retirement_attempts = 0u;
        while (!impl_->retirements.empty() &&
               retirement_attempts < impl_->retirements.size() &&
               retirement_work <
                   impl_->config.maximum_retirement_granules_per_tick)
        {
            if (impl_->retirement_cursor >= impl_->retirements.size())
                impl_->retirement_cursor = 0u;
            auto& lease = impl_->retirements[impl_->retirement_cursor];
            auto step = lease->advanceRetirementOne();
            if (!step)
                std::abort();
            if (step->work_items == 0u)
            {
                impl_->retirement_cursor =
                    (impl_->retirement_cursor + 1u) %
                    impl_->retirements.size();
                ++retirement_attempts;
                continue;
            }
            retirement_attempts = 0u;
            retirement_work += step->work_items;
            impl_->snapshot.retirement_work_items += step->work_items;
            impl_->snapshot.retired_bytes += step->bytes;
            impl_->retire_prepared_next = true;
            if (step->complete)
            {
                impl_->retirements.erase(
                    impl_->retirements.begin() +
                    static_cast<std::ptrdiff_t>(impl_->retirement_cursor));
            }
            else if (!impl_->retirements.empty())
            {
                impl_->retirement_cursor =
                    (impl_->retirement_cursor + 1u) %
                    impl_->retirements.size();
            }
        }
        while (retirement_work <
                   impl_->config.maximum_retirement_granules_per_tick &&
               impl_->backend->snapshot().retiring_regions != 0u)
        {
            auto step = impl_->backend->advanceRetirementOne();
            if (!step)
                std::abort();
            if (step->work_items == 0u)
                break;
            retirement_work += step->work_items;
            impl_->snapshot.retirement_work_items += step->work_items;
            impl_->snapshot.retired_bytes += step->bytes;
            impl_->retire_prepared_next = true;
        }
        while (retirement_work <
                   impl_->config.maximum_retirement_granules_per_tick &&
               !impl_->discarded_prepared.empty() &&
               (retirement_work == 0u ||
                (impl_->retirements.empty() &&
                 impl_->backend->snapshot().retiring_regions == 0u)))
        {
            if (!retirePreparedOne())
                break;
        }
        impl_->snapshot.maximum_staging_work_items_per_tick = std::max(
            impl_->snapshot.maximum_staging_work_items_per_tick,
            staging_work);
        impl_->snapshot.maximum_retirement_work_items_per_tick = std::max(
            impl_->snapshot.maximum_retirement_work_items_per_tick,
            retirement_work);
    }

    void Navigation3DSystem::applyPublish(
        lux::meta::EntityRegistry& registry,
        entt::entity entity,
        std::uint64_t generation) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        const auto* component = registry.valid(entity)
            ? registry.try_get<NavigationRegion3DComponent>(entity)
            : nullptr;
        if (impl_->closing || found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state != ENavigationRegion3DState::READY ||
            !found->second.lease || !component ||
            component->content != found->second.content)
        {
            ++impl_->snapshot.stale_completions;
            return;
        }
        auto& entry = found->second;
        const auto published = entry.lease->publish();
        if (!published)
        {
            entry.failure = mapFailure(published.error().code);
            entry.state = ENavigationRegion3DState::FAILED;
            ++impl_->snapshot.failed_regions;
            impl_->queueRetirement(std::move(entry.lease));
        }
        else
        {
            entry.failure = ENavigationRegion3DFailureCode::NONE;
            entry.state = ENavigationRegion3DState::ACTIVE;
        }
        registry.emplace_or_replace<NavigationRegion3DStatusComponent>(
            entity,
            NavigationRegion3DStatusComponent{
                entry.state, entry.failure, entry.generation});
    }

    void Navigation3DSystem::applyFailure(
        lux::meta::EntityRegistry& registry,
        entt::entity entity,
        std::uint64_t generation,
        ENavigationRegion3DFailureCode failure) noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (found == impl_->entries.end() || found->second.entity != entity ||
            found->second.generation != generation || !registry.valid(entity) ||
            !registry.all_of<NavigationRegion3DComponent>(entity))
        {
            ++impl_->snapshot.stale_completions;
            return;
        }
        found->second.state = ENavigationRegion3DState::FAILED;
        found->second.failure = failure;
        registry.emplace_or_replace<NavigationRegion3DStatusComponent>(
            entity,
            NavigationRegion3DStatusComponent{
                ENavigationRegion3DState::FAILED, failure, generation});
    }

    std::span<const NavigationRegion3DPrepareRequest>
    Navigation3DSystem::pendingPreparationRequests() const noexcept
    {
        return std::span<const NavigationRegion3DPrepareRequest>{
            impl_->requests}
            .subspan(impl_->request_read_offset);
    }

    void Navigation3DSystem::consumePreparationRequests(
        std::size_t count) noexcept
    {
        const auto pending =
            impl_->requests.size() - impl_->request_read_offset;
        if (count > pending)
            std::abort();
        impl_->request_read_offset += count;
        if (impl_->request_read_offset == impl_->requests.size())
        {
            impl_->requests.clear();
            impl_->request_read_offset = 0u;
        }
    }

    bool Navigation3DSystem::acceptPrepared(
        entt::entity entity,
        std::uint64_t generation,
        lux::navigation::detour3d::PreparedNavigationRegion3D&& prepared)
        noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (!prepared.valid())
        {
            ++impl_->snapshot.stale_completions;
            return false;
        }
        if (impl_->closing || found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state !=
                ENavigationRegion3DState::WAITING_BACKGROUND ||
            found->second.completion_pending ||
            prepared.requestGeneration() != generation)
        {
            if (!impl_->tryQueuePreparedRetirement(std::move(prepared)))
            {
                ++impl_->snapshot.queue_backpressure;
                return false;
            }
            ++impl_->snapshot.stale_completions;
            return false;
        }
        if (impl_->completions.size() >=
            impl_->config.maximum_pending_completions)
        {
            ++impl_->snapshot.queue_backpressure;
            return false;
        }
        const auto bytes = prepared.ownedBytes();
        if (impl_->completion_prepared_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - bytes)
        {
            std::abort();
        }
        impl_->completions.push_back(
            {entity, generation, std::move(prepared), std::nullopt});
        impl_->completion_prepared_bytes += bytes;
        found->second.completion_pending = true;
        found->second.state = ENavigationRegion3DState::STAGING;
        return true;
    }

    bool Navigation3DSystem::acceptFailure(
        entt::entity entity,
        std::uint64_t generation,
        lux::navigation::detour3d::NavigationRegion3DFailure&& failure)
        noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (impl_->closing || found == impl_->entries.end() ||
            found->second.entity != entity ||
            found->second.generation != generation ||
            found->second.state !=
                ENavigationRegion3DState::WAITING_BACKGROUND ||
            found->second.completion_pending)
        {
            ++impl_->snapshot.stale_completions;
            // A failure carries no material owner. Treat stale/closing as a
            // consumed completion so the adapter can release its request;
            // false is reserved for bounded completion backpressure.
            return true;
        }
        if (impl_->completions.size() >=
            impl_->config.maximum_pending_completions)
        {
            ++impl_->snapshot.queue_backpressure;
            return false;
        }
        impl_->completions.push_back(
            {entity, generation, std::nullopt, std::move(failure)});
        found->second.completion_pending = true;
        found->second.state = ENavigationRegion3DState::STAGING;
        return true;
    }

    lux::navigation::NavigationPathResult Navigation3DSystem::query(
        const lux::navigation::NavigationPathRequest& request) const noexcept
    {
        requireOwnerThread();
        return impl_->backend->query(request);
    }

    Navigation3DSystemSnapshot Navigation3DSystem::snapshot() const noexcept
    {
        requireOwnerThread();
        auto result = impl_->snapshot;
        result.waiting_regions = 0u;
        result.staging_regions = 0u;
        result.ready_regions = 0u;
        result.active_regions = 0u;
        for (const auto& [_, entry] : impl_->entries)
        {
            switch (entry.state)
            {
            case ENavigationRegion3DState::WAITING_BACKGROUND:
                ++result.waiting_regions;
                break;
            case ENavigationRegion3DState::STAGING:
                ++result.staging_regions;
                break;
            case ENavigationRegion3DState::READY:
                ++result.ready_regions;
                break;
            case ENavigationRegion3DState::ACTIVE:
                ++result.active_regions;
                break;
            case ENavigationRegion3DState::RETIRING:
            case ENavigationRegion3DState::RETIRED:
            case ENavigationRegion3DState::FAILED:
                break;
            }
        }
        const auto backend = impl_->backend->snapshot();
        result.generation = backend.generation;
        if (impl_->discarded_prepared.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)() -
                backend.retiring_regions))
        {
            std::abort();
        }
        result.retiring_regions = backend.retiring_regions +
            static_cast<std::uint32_t>(
                impl_->discarded_prepared.size());
        if (backend.owned_bytes >
            (std::numeric_limits<std::uint64_t>::max)() -
                impl_->discarded_prepared_bytes ||
            backend.owned_bytes + impl_->discarded_prepared_bytes >
                (std::numeric_limits<std::uint64_t>::max)() -
                    impl_->completion_prepared_bytes)
        {
            std::abort();
        }
        result.owner_bytes = backend.owned_bytes +
            impl_->discarded_prepared_bytes +
            impl_->completion_prepared_bytes;
        return result;
    }

    std::optional<NavigationRegion3DStatusComponent>
    Navigation3DSystem::status(entt::entity entity) const noexcept
    {
        const auto found = impl_->entries.find(entityKey(entity));
        if (found == impl_->entries.end() || found->second.entity != entity)
            return std::nullopt;
        return NavigationRegion3DStatusComponent{found->second.state,
                                                 found->second.failure,
                                                 found->second.generation};
    }

    void Navigation3DSystem::requestClose() noexcept
    {
        if (impl_->closing)
            return;
        impl_->closing = true;
        impl_->requests.clear();
        impl_->request_read_offset = 0u;
        for (auto& [_, entry] : impl_->entries)
            entry.close_command_pending = true;
    }

    bool Navigation3DSystem::closeComplete() const noexcept
    {
        return impl_->closeComplete();
    }

    bool Navigation3DSystem::closeNeedsOwnerTick() const noexcept
    {
        if (!impl_->closing || impl_->closeComplete())
            return false;

        // closeComplete() also audits the whole backend ledger; that safety
        // gate is not proof that update() has a local granule to execute.
        // Advertising an externally driven ledger state makes Schedule's
        // close driver spin and can starve the completion that changes it.
        // Only advertise queues and ledger states consumed by update().
        for (const auto& [_, entry] : impl_->entries)
        {
            if (entry.close_command_pending)
                return true;
        }
        if (!impl_->completions.empty() ||
            !impl_->retirements.empty() ||
            !impl_->discarded_prepared.empty())
        {
            return true;
        }
        return impl_->backend->snapshot().retiring_regions != 0u;
    }
} // namespace lux::ecs
