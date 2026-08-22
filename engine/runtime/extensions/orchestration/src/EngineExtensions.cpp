#include <lux/engine/runtime/extensions/EngineExtensions.hpp>
#include <lux/engine/runtime/extensions/EngineExtensionsCloseSender.hpp>
#include <lux/engine/runtime/extensions/ModuleMetadataBatch.hpp>

#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <moodycamel/concurrentqueue.h>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <optional>
#include <ranges>
#include <utility>

namespace lux::extensions
{
    namespace ex = stdexec;

    namespace
    {
        using TicketPublisher = OperationTicketPublisher<
            EExtensionLoadPhase,
            EExtensionLoadError,
            ExtensionLoadResult>;

        [[nodiscard]] EExtensionLoadError mapLoadFailure(
            const ExtensionModuleLoadFailure& failure) noexcept
        {
            if (failure.code ==
                EExtensionModuleLoadError::MANIFEST_ID_MISMATCH)
                return EExtensionLoadError::ID_COLLISION;
            return EExtensionLoadError::LIBRARY_LOAD_FAILED;
        }

        [[nodiscard]] EExtensionLoadError mapCommitFailure(
            const ExtensionModuleCommitFailure& failure) noexcept
        {
            switch (failure.code)
            {
            case EExtensionModuleCommitError::MISSING_DEPENDENCY:
                return EExtensionLoadError::DEPENDENCY_MISSING;
            case EExtensionModuleCommitError::DEPENDENCY_NOT_READY:
                return EExtensionLoadError::DEPENDENCY_NOT_READY;
            case EExtensionModuleCommitError::DEPENDENCY_VERSION_MISMATCH:
                return EExtensionLoadError::DEPENDENCY_VERSION_MISMATCH;
            case EExtensionModuleCommitError::DEPENDENCY_CYCLE:
                return EExtensionLoadError::DEPENDENCY_CYCLE;
            default:
                return EExtensionLoadError::MODULE_COMMIT_FAILED;
            }
        }

        [[nodiscard]] bool sameRequirement(
            const ExtensionModuleRequirement& lhs,
            const ExtensionModuleRequirement& rhs) noexcept
        {
            return sameStableId(lhs.id.view(), rhs.id.view()) &&
                lhs.source.kind() == rhs.source.kind() &&
                lhs.source.path() == rhs.source.path() &&
                lhs.source.image().data() == rhs.source.image().data() &&
                lhs.source.image().size() == rhs.source.image().size() &&
                lhs.source.hint() == rhs.source.hint() &&
                lhs.target == rhs.target &&
                lhs.required_major == rhs.required_major &&
                lhs.minimum_minor == rhs.minimum_minor;
        }

        struct ExtensionLoadCommand final
        {
            ExtensionLoadCommand(
                ExtensionModuleRequirement value_requirement,
                std::uint64_t generation)
                : requirement(std::move(value_requirement))
                , accounted_bytes(requirement.source.accountedBytes())
                , publisher(EExtensionLoadPhase::QUEUED, generation)
            {}

            ExtensionModuleRequirement requirement;
            std::size_t accounted_bytes{0u};
            TicketPublisher publisher;
        };
    }

    struct EngineExtensions::Impl final
        : public std::enable_shared_from_this<Impl>
    {
        struct Endpoint final
        {
            using RequirementList =
                std::vector<ExtensionModuleRequirement>;

            Endpoint(
                std::vector<ExtensionModuleRequirement> value_requirements,
                EngineExtensionQueueConfig value_config,
                lux::exec::MainThreadDispatcher value_dispatcher)
                : requirements(std::make_shared<const RequirementList>(
                      std::move(value_requirements)))
                , config(value_config)
                , queue(std::max<std::size_t>(config.capacity, 1u))
                , dispatcher(std::move(value_dispatcher))
            {
                config.capacity = std::max<std::size_t>(config.capacity, 1u);
            }

            [[nodiscard]] std::optional<ExtensionModuleRequirement>
            findRequirement(
                ExtensionIdView id,
                bool& collision) const noexcept
            {
                collision = false;
                const auto snapshot = std::atomic_load_explicit(
                    &requirements,
                    std::memory_order_acquire);
                for (const auto& requirement : *snapshot)
                {
                    if (requirement.id.hash() != id.hash())
                        continue;
                    if (requirement.id.name() == id.name())
                        return requirement;
                    collision = true;
                }
                return std::nullopt;
            }

            [[nodiscard]] lux::cxx::expected<
                void,
                EExtensionRequirementUpdateError>
            appendRequirements(
                std::vector<ExtensionModuleRequirement> additions) noexcept
            {
                if (!admission_open.load(std::memory_order_acquire))
                {
                    return lux::cxx::unexpected(
                        EExtensionRequirementUpdateError::STOPPING);
                }

                auto current = std::atomic_load_explicit(
                    &requirements,
                    std::memory_order_acquire);
                for (;;)
                {
                    auto next = std::make_shared<RequirementList>(*current);
                    for (const auto& addition : additions)
                    {
                        if (!addition.id.isValid() ||
                            !addition.source.valid() ||
                            !isCanonicalStableName(addition.id.name()))
                        {
                            return lux::cxx::unexpected(
                                EExtensionRequirementUpdateError::
                                    INVALID_REQUIREMENT);
                        }

                        const auto same_hash = std::ranges::find_if(
                            *next,
                            [&addition](const auto& existing) noexcept
                            {
                                return existing.id.hash() ==
                                    addition.id.hash();
                            });
                        if (same_hash == next->end())
                        {
                            next->push_back(addition);
                            continue;
                        }
                        if (!sameStableId(
                                same_hash->id.view(),
                                addition.id.view()))
                        {
                            return lux::cxx::unexpected(
                                EExtensionRequirementUpdateError::ID_COLLISION);
                        }
                        if (!sameRequirement(*same_hash, addition))
                        {
                            return lux::cxx::unexpected(
                                EExtensionRequirementUpdateError::
                                    CONFLICTING_REQUIREMENT);
                        }
                    }

                    std::shared_ptr<const RequirementList> published =
                        std::move(next);
                    if (std::atomic_compare_exchange_weak_explicit(
                            &requirements,
                            &current,
                            std::move(published),
                            std::memory_order_release,
                            std::memory_order_acquire))
                        return {};
                }
            }

            void requestWake() noexcept
            {
                if (wake_pending.exchange(true, std::memory_order_acq_rel))
                    return;
                auto weak = weak_from_this;
                if (!dispatcher.tryDispatchToMainThread(
                        [weak]() noexcept
                        {
                            const auto endpoint = weak.lock();
                            if (!endpoint)
                                return;
                            endpoint->wake_pending.store(
                                false,
                                std::memory_order_release);
                            if (auto owner = endpoint->owner.lock())
                                (void)owner->process(8u);
                        }))
                {
                    wake_pending.store(false, std::memory_order_release);
                }
            }

            [[nodiscard]] ExtensionLoadTicket submit(ExtensionIdView id)
            {
                const auto generation = next_generation.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                bool collision = false;
                auto requirement = findRequirement(id, collision);
                if (!requirement)
                {
                    TicketPublisher publisher(
                        EExtensionLoadPhase::QUEUED,
                        generation);
                    auto ticket = publisher.ticket();
                    publisher.fail(
                        collision ? EExtensionLoadError::ID_COLLISION
                                  : EExtensionLoadError::UNKNOWN_EXTENSION);
                    return ticket;
                }

                ExtensionLoadCommand command{std::move(*requirement), generation};
                auto ticket = command.publisher.ticket();
                if (!admission_open.load(std::memory_order_acquire))
                {
                    command.publisher.fail(EExtensionLoadError::STOPPING);
                    return ticket;
                }

                auto bytes = bytes_inflight.load(std::memory_order_relaxed);
                for (;;)
                {
                    if (command.accounted_bytes > config.byte_budget -
                            std::min(bytes, config.byte_budget))
                    {
                        command.publisher.fail(
                            EExtensionLoadError::BYTE_BUDGET_EXHAUSTED);
                        return ticket;
                    }
                    if (bytes_inflight.compare_exchange_weak(
                            bytes,
                            bytes + command.accounted_bytes,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                        break;
                }

                const auto accounted = command.accounted_bytes;
                auto terminal = command.publisher;
                const auto count = queued.fetch_add(
                    1u,
                    std::memory_order_acq_rel);
                if (count >= config.capacity ||
                    !queue.try_enqueue(std::move(command)))
                {
                    queued.fetch_sub(1u, std::memory_order_release);
                    bytes_inflight.fetch_sub(
                        accounted,
                        std::memory_order_release);
                    terminal.fail(EExtensionLoadError::QUEUE_FULL);
                    return ticket;
                }
                requestWake();
                return ticket;
            }

            // Use the shared_ptr atomic free functions instead of
            // atomic<shared_ptr>. Android libc++ only enables the class
            // specialization in newer API/library combinations, while the
            // free-function protocol is available on every supported host.
            std::shared_ptr<const RequirementList> requirements;
            EngineExtensionQueueConfig config;
            moodycamel::ConcurrentQueue<ExtensionLoadCommand> queue;
            lux::exec::MainThreadDispatcher dispatcher;
            std::weak_ptr<Impl> owner;
            std::weak_ptr<Endpoint> weak_from_this;
            std::atomic<std::size_t> queued{0u};
            std::atomic<std::size_t> bytes_inflight{0u};
            std::atomic<std::uint64_t> next_generation{1u};
            std::atomic<bool> admission_open{true};
            std::atomic<bool> wake_pending{false};
        };

        struct Active final
        {
            struct PendingMetadata final
            {
                ExtensionId module;
                ModuleMetadataBatch batch;
            };

            explicit Active(ExtensionLoadCommand value)
                : command(std::move(value))
            {}

            ExtensionLoadCommand command;
            std::vector<PreparedExtensionModule> prepared_modules;
            std::vector<ExtensionModuleRequirement> pending_requirements;
            std::vector<ModuleLease> modules_to_register;
            std::vector<PendingMetadata> pending_metadata;
            std::size_t registration_index{0u};
            std::optional<ModuleMetadataBatch> metadata_batch;
            std::unique_ptr<EditorRegistrationTransaction> editor_draft;
        };

        Impl(
            EngineExtensionServices value_services,
            std::vector<ExtensionModuleRequirement> requirements,
            EngineExtensionQueueConfig queue)
            : services(std::move(value_services))
            , scope(services.async)
            , endpoint(std::make_shared<Endpoint>(
                  std::move(requirements),
                  queue,
                  services.async.mainThreadDispatcher()))
        {}

        void bind() noexcept
        {
            endpoint->owner = shared_from_this();
            endpoint->weak_from_this = endpoint;
        }

        void releaseAccounting(const ExtensionLoadCommand& command) noexcept
        {
            endpoint->bytes_inflight.fetch_sub(
                command.accounted_bytes,
                std::memory_order_release);
        }

        void finishFailure(EExtensionLoadError error) noexcept
        {
            if (!active)
                return;
            active->command.publisher.fail(error);
            releaseAccounting(active->command);
            active.reset();
            endpoint->requestWake();
        }

        void finishSuccess(ModuleLease module) noexcept
        {
            if (!active || !module)
                return;
            active->command.publisher.setPhase(EExtensionLoadPhase::READY);
            active->command.publisher.succeed(ExtensionLoadResult{
                module->id(),
                module->version()});
            releaseAccounting(active->command);
            active.reset();
            endpoint->requestWake();
        }

        void publishRegistration(ModuleLease module) noexcept
        {
            if (!active)
                return;
            active->command.publisher.setPhase(
                EExtensionLoadPhase::COMMITTING_REGISTRATION);
            if (!active->metadata_batch)
                std::terminate();
            active->metadata_batch->commit();
            active->metadata_batch.reset();
            if (active->editor_draft)
            {
                auto editor = active->editor_draft->commit();
                active->editor_draft.reset();
                if (!editor)
                {
                    std::terminate();
                }
            }
            if (!services.modules.markReady(module->id().view()))
                std::terminate();
            if (services.events)
            {
                services.events->publish(ExtensionModuleLoaded{
                    module->id(),
                    module->version()});
            }

            ++active->registration_index;
            active->metadata_batch.reset();
            active->editor_draft.reset();
            if (active->registration_index <
                active->modules_to_register.size())
            {
                collectRegistration(
                    active->modules_to_register[active->registration_index]);
                return;
            }

            auto root = services.modules.find(
                active->command.requirement.id.view());
            if (!root)
            {
                finishFailure(EExtensionLoadError::MODULE_COMMIT_FAILED);
                return;
            }
            finishSuccess(std::move(root));
        }

        void collectRegistration(ModuleLease module) noexcept
        {
            if (!active || !module)
                return;
            active->command.publisher.setPhase(
                EExtensionLoadPhase::COLLECTING_REGISTRATION);
            const auto entrypoints = services.modules.entrypoints(
                module->id().view());

            const auto metadata = std::ranges::find_if(
                active->pending_metadata,
                [&module](const Active::PendingMetadata& pending) noexcept
                {
                    return sameStableId(
                        pending.module.view(),
                        module->id().view());
                });
            if (metadata == active->pending_metadata.end())
            {
                (void)services.modules.markFailed(module->id().view());
                finishFailure(EExtensionLoadError::REGISTRATION_FAILED);
                return;
            }
            active->metadata_batch.emplace(std::move(metadata->batch));
            active->pending_metadata.erase(metadata);
            if (entrypoints.editor)
            {
                if (!services.prepare_editor)
                {
                    (void)services.modules.markFailed(module->id().view());
                    finishFailure(EExtensionLoadError::REGISTRATION_FAILED);
                    return;
                }
                auto prepared = services.prepare_editor(entrypoints);
                if (!prepared || !*prepared)
                {
                    (void)services.modules.markFailed(module->id().view());
                    finishFailure(EExtensionLoadError::REGISTRATION_FAILED);
                    return;
                }
                active->editor_draft = std::move(*prepared);
            }

            if (active->editor_draft)
            {
                if (auto checked = active->editor_draft->validate(); !checked)
                {
                    (void)services.modules.markFailed(module->id().view());
                    finishFailure(
                        EExtensionLoadError::CATALOG_VALIDATION_FAILED);
                    return;
                }
            }
            publishRegistration(std::move(module));
        }

        void onPrepared(lux::cxx::expected<
            PreparedExtensionModule,
            ExtensionModuleLoadFailure> prepared) noexcept
        {
            if (!active)
                return;
            if (!prepared)
            {
                finishFailure(mapLoadFailure(prepared.error()));
                return;
            }
            active->command.publisher.setPhase(
                EExtensionLoadPhase::VALIDATING_MODULE);
            auto metadata = ModuleMetadataBatch::prepare(
                prepared->lease(),
                services.components);
            if (!metadata)
            {
                finishFailure(EExtensionLoadError::CATALOG_VALIDATION_FAILED);
                return;
            }
            const auto prepared_id = prepared->module().id().view();
            active->pending_metadata.push_back(Active::PendingMetadata{
                prepared->module().id(),
                std::move(*metadata)});
            for (const auto& dependency : prepared->dependencies())
            {
                const auto status = services.modules.requirementStatus(
                    dependency.id.view(),
                    dependency.required_major,
                    dependency.minimum_minor);
                if (status == EExtensionRequirementStatus::READY)
                    continue;
                if (status == EExtensionRequirementStatus::VERSION_MISMATCH)
                {
                    finishFailure(
                        EExtensionLoadError::DEPENDENCY_VERSION_MISMATCH);
                    return;
                }
                if (status == EExtensionRequirementStatus::ID_COLLISION)
                {
                    finishFailure(EExtensionLoadError::ID_COLLISION);
                    return;
                }
                if (status == EExtensionRequirementStatus::NOT_READY)
                {
                    finishFailure(EExtensionLoadError::DEPENDENCY_NOT_READY);
                    return;
                }

                const auto already_prepared = std::ranges::any_of(
                    active->prepared_modules,
                    [&dependency](const PreparedExtensionModule& item)
                    {
                        return sameStableId(
                            item.module().id().view(),
                            dependency.id.view());
                    });
                const auto already_pending = std::ranges::any_of(
                    active->pending_requirements,
                    [&dependency](const ExtensionModuleRequirement& item)
                    {
                        return sameStableId(
                            item.id.view(), dependency.id.view());
                    });
                if (already_prepared || already_pending ||
                    sameStableId(prepared_id, dependency.id.view()))
                    continue;

                bool collision = false;
                auto requirement = endpoint->findRequirement(
                    dependency.id.view(), collision);
                if (!requirement)
                {
                    finishFailure(
                        collision ? EExtensionLoadError::ID_COLLISION
                                  : EExtensionLoadError::DEPENDENCY_MISSING);
                    return;
                }
                active->pending_requirements.push_back(
                    std::move(*requirement));
            }
            active->prepared_modules.push_back(std::move(*prepared));
            if (!active->pending_requirements.empty())
            {
                startNextPreparation();
                return;
            }

            auto committed = services.modules.commitBatch(
                std::move(active->prepared_modules));
            if (!committed || committed->empty())
            {
                finishFailure(
                    committed
                        ? EExtensionLoadError::MODULE_COMMIT_FAILED
                        : mapCommitFailure(committed.error()));
                return;
            }
            active->modules_to_register = std::move(*committed);
            active->registration_index = 0u;
            for (const auto& module : active->modules_to_register)
            {
                if (!services.modules.beginRegistration(module->id().view()))
                {
                    finishFailure(EExtensionLoadError::MODULE_COMMIT_FAILED);
                    return;
                }
            }
            collectRegistration(active->modules_to_register.front());
        }

        void startNextPreparation() noexcept
        {
            if (!active || active->pending_requirements.empty())
                return;
            active->command.publisher.setPhase(
                EExtensionLoadPhase::LOADING_LIBRARY);
            auto requirement = std::move(
                active->pending_requirements.back());
            active->pending_requirements.pop_back();
            auto sender = ex::schedule(
                    lux::exec::blockingIoScheduler(services.async))
                | ex::then(
                      [requirement = std::move(requirement)]() noexcept
                      {
                          return ExtensionModuleManager::prepare(requirement);
                      })
                | ex::continues_on(
                      lux::exec::mainThreadScheduler(services.async))
                | ex::then(
                      [self = shared_from_this()](auto prepared) noexcept
                      {
                          self->onPrepared(std::move(prepared));
                      })
                | ex::upon_stopped(
                      [self = shared_from_this()]() noexcept
                      {
                          self->finishFailure(EExtensionLoadError::STOPPING);
                      });
            if (!lux::exec::spawn(scope, std::move(sender)))
                finishFailure(EExtensionLoadError::STOPPING);
        }

        void startLoad() noexcept
        {
            active->pending_requirements.push_back(
                active->command.requirement);
            startNextPreparation();
        }

        [[nodiscard]] std::size_t process(std::size_t budget) noexcept
        {
            if (active || budget == 0u)
                return 0u;
            ExtensionLoadCommand command{
                ExtensionModuleRequirement{},
                0u};
            if (!endpoint->queue.try_dequeue(command))
                return 0u;
            endpoint->queued.fetch_sub(1u, std::memory_order_release);

            const auto status = services.modules.requirementStatus(
                command.requirement.id.view(),
                command.requirement.required_major,
                command.requirement.minimum_minor);
            if (status == EExtensionRequirementStatus::READY)
            {
                auto module = services.modules.find(
                    command.requirement.id.view());
                command.publisher.setPhase(EExtensionLoadPhase::READY);
                command.publisher.succeed(ExtensionLoadResult{
                    module->id(), module->version()});
                releaseAccounting(command);
                endpoint->requestWake();
                return 1u;
            }
            if (status != EExtensionRequirementStatus::MISSING)
            {
                command.publisher.fail(
                    status == EExtensionRequirementStatus::ID_COLLISION
                        ? EExtensionLoadError::ID_COLLISION
                        : EExtensionLoadError::DEPENDENCY_NOT_READY);
                releaseAccounting(command);
                endpoint->requestWake();
                return 1u;
            }

            active.emplace(std::move(command));
            startLoad();
            return 1u;
        }

        void beginClose(
            lux::cxx::move_only_function<void(EngineExtensionsCloseReport)>
                completion) noexcept
        {
            if (close_complete)
            {
                if (completion)
                    completion(close_report);
                return;
            }
            if (completion)
                close_waiters.push_back(std::move(completion));
            if (closing)
                return;

            closing = true;
            endpoint->admission_open.store(false, std::memory_order_release);
            endpoint->owner.reset();

            ExtensionLoadCommand command{
                ExtensionModuleRequirement{},
                0u};
            while (endpoint->queue.try_dequeue(command))
            {
                endpoint->queued.fetch_sub(1u, std::memory_order_release);
                command.publisher.fail(EExtensionLoadError::STOPPING);
                releaseAccounting(command);
                ++close_report.rejected_queued;
            }
            close_report.pending_loads = active ? 1u : 0u;

            auto self = shared_from_this();
            lux::exec::detail::subscribeScopeClose(
                scope,
                [self = std::move(self)]() noexcept
                {
                    self->finishClose();
                });
        }

        void finishClose() noexcept
        {
            if (close_complete)
                return;
            if (active)
                finishFailure(EExtensionLoadError::STOPPING);
            close_report.pending_loads = 0u;
            completeClose();
        }

        void completeClose() noexcept
        {
            if (close_complete)
                return;
            close_complete = true;

            auto waiters = std::move(close_waiters);
            close_waiters.clear();
            for (auto& waiter : waiters)
                if (waiter)
                    waiter(close_report);
        }

        EngineExtensionServices services;
        lux::exec::AsyncScope scope;
        std::shared_ptr<Endpoint> endpoint;
        std::optional<Active> active;
        EngineExtensionsCloseReport close_report;
        std::vector<lux::cxx::move_only_function<
            void(EngineExtensionsCloseReport)>> close_waiters;
        bool closing{false};
        bool close_complete{false};
    };

    EngineExtensions::EngineExtensions(
        EngineExtensionServices services,
        std::vector<ExtensionModuleRequirement> requirements,
        EngineExtensionQueueConfig queue)
        : impl_(std::make_shared<Impl>(
              std::move(services),
              std::move(requirements),
              queue))
    {
        impl_->bind();
    }

    EngineExtensions::~EngineExtensions() noexcept
    {
        if (impl_)
            impl_->beginClose({});
    }

    ExtensionLoadTicket EngineExtensions::requestLoad(
        ExtensionIdView id) const
    {
        if (impl_)
            return impl_->endpoint->submit(id);
        TicketPublisher publisher(EExtensionLoadPhase::QUEUED, 0u);
        auto ticket = publisher.ticket();
        publisher.fail(EExtensionLoadError::STOPPING);
        return ticket;
    }

    lux::cxx::expected<void, EExtensionRequirementUpdateError>
    EngineExtensions::addRequirements(
        std::vector<ExtensionModuleRequirement> requirements) noexcept
    {
        if (!impl_)
        {
            return lux::cxx::unexpected(
                EExtensionRequirementUpdateError::STOPPING);
        }
        return impl_->endpoint->appendRequirements(std::move(requirements));
    }

    std::size_t EngineExtensions::processSafePoint(std::size_t budget) noexcept
    {
        return impl_ ? impl_->process(budget) : 0u;
    }

    std::vector<ExtensionModuleSnapshot> EngineExtensions::snapshot() const
    {
        return impl_ ? impl_->services.modules.snapshot()
                     : std::vector<ExtensionModuleSnapshot>{};
    }

    EngineExtensionsSnapshot EngineExtensions::runtimeSnapshot() const
    {
        EngineExtensionsSnapshot value;
        if (!impl_)
            return value;
        value.modules = impl_->services.modules.snapshot();
        value.queued_commands = impl_->endpoint->queued.load(
            std::memory_order_acquire);
        value.accounted_bytes = impl_->endpoint->bytes_inflight.load(
            std::memory_order_acquire);
        value.admission_open = impl_->endpoint->admission_open.load(
            std::memory_order_acquire);
        value.closing = impl_->closing;
        if (impl_->active)
        {
            value.active_operation =
                impl_->active->command.publisher.ticket().snapshot();
        }
        return value;
    }

    EngineExtensionsCloseSender EngineExtensions::closeAsync() noexcept
    {
        return EngineExtensionsCloseSender{*this};
    }

    LUX_RUNTIME_EXTENSION_ORCHESTRATION_PUBLIC void subscribeEngineExtensionsClose(
        EngineExtensions& owner,
        lux::cxx::move_only_function<void(EngineExtensionsCloseReport)>
            completion) noexcept
    {
        if (!owner.impl_)
        {
            completion(EngineExtensionsCloseReport{});
            return;
        }
        owner.impl_->beginClose(std::move(completion));
    }
}
